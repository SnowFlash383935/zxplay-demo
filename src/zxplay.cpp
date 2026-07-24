/**
 * zxplay.cpp
 *
 * Минимальный CLI-плеер поверх ZXTune (https://github.com/vitamin-caig/zxtune).
 * Играет что угодно из зарегистрированных в core_plugins_players форматов,
 * в частности .spc и .minigsf (с автоматическим подтягиванием GSFLIB).
 *
 * Сборка: см. Makefile-рецепт из предыдущего ответа (на основе apps/zxtune123/Makefile),
 * либо ручками — этот файл нужно линковать против собранных модулей:
 *   analysis async binary binary_compression binary_format
 *   core core_plugins_players core_plugins_archives
 *   devices_* formats_* io l10n_stub
 *   module_conversion module_players module_properties
 *   parameters platform platform_application platform_version
 *   sound sound_backends strings tools
 * + 3rdparty (для наших форматов достаточно: snesspc, mgba, zlib, lzma и т.д. —
 *   но раз уж "full" сборка, тащи весь список 3rdparty из прошлого ответа).
 *
 * usage: zxplay <file.spc|file.gsf|file.minigsf> [--loop] [--loop-limit=N] [--seek=SEC] [--output=FILE.wav]
 *
 * Без --output играет живьём через системный звук (ALSA/Pulse/...).
 * С --output рендерит в .wav-файл на диск (для headless CI и подобного) —
 * см. sound/backends/wav_backend.cpp, бэкенд "wav" + Sound::CreateFileService().
 */

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

#include "binary/container_factories.h"
#include "core/additional_files_resolve.h"
#include "core/service.h"
#include "error.h"
#include "module/additional_files.h"
#include "module/attributes.h"
#include "module/holder.h"
#include "module/information.h"
#include "parameters/container.h"
#include "sound/backends_parameters.h"
#include "sound/service.h"
#include "sound/sound_parameters.h"
#include "time/instant.h"
#include "platform/version/api.h"
namespace Platform::Version
{
  const StringView PROGRAM_NAME = "ZXPlay";
}

namespace fs = std::filesystem;
using namespace std::string_view_literals;

namespace
{
  // Небольшой хелпер: StringView библиотеки -> std::string, без гаданий про operator<<
  std::string ToStdString(StringView sv)
  {
    return std::string(sv.data(), sv.size());
  }

  // ---- читаем файл целиком в память ----
  Binary::Dump ReadFile(const fs::path& path)
  {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
      throw std::runtime_error("Cannot open file: " + path.string());
    }
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    Binary::Dump buf(size);
    if (size && !in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size)))
    {
      throw std::runtime_error("Failed to read file: " + path.string());
    }
    return buf;
  }

  Binary::Container::Ptr LoadContainer(const fs::path& path)
  {
    return Binary::CreateContainer(std::make_unique<Binary::Dump>(ReadFile(path)));
  }

  // ---- источник для доразрешения "довесков" (GSFLIB для miniGSF) ----
  // ZXTune сам знает, чего ему не хватает (Enumerate()), мы просто
  // читаем недостающие файлы из той же папки, что и основной файл.
  class DirectoryFilesSource : public Module::AdditionalFilesSource
  {
  public:
    explicit DirectoryFilesSource(fs::path baseDir)
      : BaseDir(std::move(baseDir))
    {}

    Binary::Container::Ptr Get(StringView name) const override
    {
      const auto filename = ToStdString(name);
      const auto candidate = BaseDir / filename;
      std::cerr << "  resolving additional file: " << filename << " -> " << candidate.string() << "\n";
      return LoadContainer(candidate);
    }

  private:
    const fs::path BaseDir;
  };

  // Для miniGSF (и всего xSF-семейства) Holder дополнительно реализует
  // Module::AdditionalFiles. Resolve() у него не const, а Holder мы держим
  // как shared_ptr<const Holder> — так же поступает и сама библиотека
  // (см. apps/zxtune-android/.../jni/module.cpp), поэтому тут честный const_cast.
  void ResolveAdditionalFilesIfNeeded(const Module::Holder::Ptr& holder, const fs::path& baseDir)
  {
    auto* mutableHolder = const_cast<Module::Holder*>(holder.get());
    if (auto* files = dynamic_cast<Module::AdditionalFiles*>(mutableHolder))
    {
      const auto missing = files->Enumerate();
      if (!missing.empty())
      {
        std::cerr << "Module references " << missing.size()
                  << " additional file(s) (miniGSF -> GSFLIB and friends), resolving from: " << baseDir.string()
                  << "\n";
        const DirectoryFilesSource source(baseDir);
        Module::ResolveAdditionalFiles(source, *files);
      }
    }
  }

  // ---- ждём естественного конца воспроизведения ----
  class WaitForFinish : public Sound::BackendCallback
  {
  public:
    void OnStart() override {}
    void OnFrame(const Module::State&) override {}
    void OnStop() override {}
    void OnPause() override {}
    void OnResume() override {}

    void OnFinish() override
    {
      {
        const std::lock_guard<std::mutex> lk(Mtx);
        Done = true;
      }
      Cv.notify_one();
    }

    void Wait()
    {
      std::unique_lock<std::mutex> lk(Mtx);
      Cv.wait(lk, [this] { return Done; });
    }

  private:
    std::mutex Mtx;
    std::condition_variable Cv;
    bool Done = false;
  };

  struct Options
  {
    fs::path Input;
    fs::path Output;  // если задан --output=, играем не в звук, а рендерим .wav на диск
    bool Loop = false;
    unsigned LoopLimit = 0;  // 0 = крутить бесконечно (Ctrl+C, чтобы остановить)
    unsigned SeekSeconds = 0;
  };

  Options ParseArgs(int argc, char* argv[])
  {
    Options opts;
    for (int i = 1; i < argc; ++i)
    {
      const std::string arg = argv[i];
      if (arg == "--loop")
      {
        opts.Loop = true;
      }
      else if (arg.rfind("--loop-limit=", 0) == 0)
      {
        opts.Loop = true;
        opts.LoopLimit = static_cast<unsigned>(std::stoul(arg.substr(13)));
      }
      else if (arg.rfind("--seek=", 0) == 0)
      {
        opts.SeekSeconds = static_cast<unsigned>(std::stoul(arg.substr(7)));
      }
      else if (arg.rfind("--output=", 0) == 0)
      {
        opts.Output = arg.substr(9);
      }
      else if (!arg.empty() && arg[0] != '-')
      {
        opts.Input = arg;
      }
      else
      {
        throw std::runtime_error("Unknown option: " + arg);
      }
    }
    if (opts.Input.empty())
    {
      throw std::runtime_error(
          "usage: zxplay <file.spc|file.gsf|file.minigsf> [--loop] [--loop-limit=N] [--seek=SEC] [--output=FILE.wav]");
    }
    return opts;
  }
}  // namespace

int main(int argc, char* argv[])
{
  try
  {
    const auto opts = ParseArgs(argc, argv);

    // 1. Файл -> Binary::Container
    const auto data = LoadContainer(opts.Input);

    // 2. Ядро: единая точка входа что для .spc, что для .gsf, что для чего угодно ещё
    const auto coreParams = Parameters::Container::Create();
    const auto coreService = ZXTune::Service::Create(coreParams);

    const auto initialProps = Parameters::Container::Create();
    // subpath пустой: это одиночный файл, а не архив/мультитрек-контейнер
    const auto holder = coreService->OpenModule(data, {}, initialProps);

    // 3. "Дырявые" форматы (miniGSF без вшитого GSFLIB) дошиваем сами
    ResolveAdditionalFilesIfNeeded(holder, opts.Input.parent_path());

    // 4. Метадата + инфа про луп — работает одинаково для любого формата,
    //    потому что Module::Information / Module::Holder — общий интерфейс
    const auto props = holder->GetModuleProperties();
    const auto info = holder->GetModuleInformation();

    std::cout << "Title:    " << Parameters::GetString(*props, Module::ATTR_TITLE, "<unknown>"sv) << "\n"
              << "Author:   " << Parameters::GetString(*props, Module::ATTR_AUTHOR, "<unknown>"sv) << "\n"
              << "Duration: " << info->Duration().Get() << " ms\n";

    const auto loopMs = info->LoopDuration().Get();
    if (loopMs > 0)
    {
      const auto loopStartMs = info->Duration().Get() - loopMs;
      std::cout << "Loop:     yes, starts at " << loopStartMs << " ms (loop part = " << loopMs << " ms)\n";
    }
    else
    {
      std::cout << "Loop:     no (one-shot track)\n";
    }

    // 5. Параметры звука. LOOPED/LOOP_LIMIT настраиваются ЗДЕСЬ, один раз,
    //    и применяются к любому формату одинаково (см. module/players/pipeline.cpp -
    //    PipelinedRenderer оборачивает CreateRenderer() любого Holder'а извне)
    const auto soundParams = Parameters::Container::Create();
    soundParams->SetValue(Parameters::ZXTune::Sound::FREQUENCY, Parameters::ZXTune::Sound::FREQUENCY_DEFAULT);
    if (opts.Loop)
    {
      soundParams->SetValue(Parameters::ZXTune::Sound::LOOPED, 1);
      if (opts.LoopLimit)
      {
        soundParams->SetValue(Parameters::ZXTune::Sound::LOOP_LIMIT, opts.LoopLimit);
      }
      std::cout << "Looping:  enabled" << (opts.LoopLimit ? (" (limit=" + std::to_string(opts.LoopLimit) + ")") : " (infinite)")
                << "\n";
    }

    const auto callback = std::make_shared<WaitForFinish>();
    Sound::Backend::Ptr backend;

    if (!opts.Output.empty())
    {
      // Рендер в файл: никакого живого звука, ZXTune просто гонит рендер
      // так быстро, как процессор считает семплы, и льёт PCM в .wav
      soundParams->SetValue(Parameters::ZXTune::Sound::Backends::File::FILENAME, opts.Output.string());
      const auto fileService = Sound::CreateFileService(soundParams);
      backend = fileService->CreateBackend(Sound::BackendId::FromString("wav"), holder, callback);
      std::cerr << "Rendering to: " << opts.Output.string() << "\n";
    }
    else
    {
      // Живой звук: перебираем доступные системные бэкенды (ALSA/PulseAudio/...),
      // берём первый, который реально смог инициализироваться — как в apps/zxtune123/sound.cpp
      const auto soundService = Sound::CreateSystemService(soundParams);
      Error lastError;
      for (const auto id : soundService->GetAvailableBackends())
      {
        try
        {
          backend = soundService->CreateBackend(id, holder, callback);
          std::cerr << "Using backend: " << ToStdString(id) << "\n";
          break;
        }
        catch (const Error& e)
        {
          lastError = e;
          std::cerr << "Backend '" << ToStdString(id) << "' unavailable: " << e.GetText() << "\n";
        }
      }
      if (!backend)
      {
        throw std::runtime_error("No working sound backend found: " + lastError.GetText());
      }
    }

    // 7. Play (+ опциональный seek)
    const auto control = backend->GetPlaybackControl();
    if (opts.SeekSeconds)
    {
      control->SetPosition(Time::AtMillisecond(opts.SeekSeconds * 1000ull));
    }
    control->Play();

    // 8. Ждём OnFinish(): естественный конец трека, либо конец лупов при --loop-limit.
    //    При --loop без лимита это будет играть, пока не прибьёшь процесс.
    callback->Wait();
  }
  catch (const std::exception& e)
  {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
  catch (const Error& e)
  {
    std::cerr << "ZXTune error: " << e.GetText() << "\n";
    return 1;
  }
  return 0;
}
