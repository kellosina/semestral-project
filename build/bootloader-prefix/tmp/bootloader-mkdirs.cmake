# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "D:/EspressifIDF/components/bootloader/subproject"
  "D:/EspressifIDF/workspace/bridge/build/bootloader"
  "D:/EspressifIDF/workspace/bridge/build/bootloader-prefix"
  "D:/EspressifIDF/workspace/bridge/build/bootloader-prefix/tmp"
  "D:/EspressifIDF/workspace/bridge/build/bootloader-prefix/src/bootloader-stamp"
  "D:/EspressifIDF/workspace/bridge/build/bootloader-prefix/src"
  "D:/EspressifIDF/workspace/bridge/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/EspressifIDF/workspace/bridge/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/EspressifIDF/workspace/bridge/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
