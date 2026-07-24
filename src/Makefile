binary_name := zxplay
dirs.root := ../..
source_files := zxplay.cpp

libraries.common = analysis async \
                  binary binary_compression binary_format \
                  core core_plugins_players core_plugins_archives \
                  debug devices_aym devices_beeper devices_dac devices_aym_dumper devices_fm devices_saa devices_z80 \
                  formats_archived formats_archived_multitrack formats_chiptune formats_packed formats_multitrack \
                  io l10n_stub \
                  module_conversion module_players module_properties \
                  parameters platform platform_application platform_version \
                  sound sound_backends strings tools

libraries.3rdparty = asap atrac9 ffmpeg FLAC gme he ht hvl lazyusf2 lhasa lzma mgba mpg123 \
                     ogg openmpt opus sidplayfp snesspc sseqplayer unrar v2m vio2sf vgm \
                     vgmstream vorbis xmp z80ex zlib

libraries.boost = program_options

embedded_files = $(l10n_archive)
include $(dirs.root)/makefile.mak
include $(dirs.root)/make/package.mak
include $(dirs.root)/samples/samples.mak
include $(dirs.root)/3rdparty/install.mak
