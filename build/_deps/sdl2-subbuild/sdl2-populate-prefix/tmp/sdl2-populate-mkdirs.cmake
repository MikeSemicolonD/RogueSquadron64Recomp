# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "E:/Projects/RogueSquadron64Recomp/build/_deps/sdl2-src"
  "E:/Projects/RogueSquadron64Recomp/build/_deps/sdl2-build"
  "E:/Projects/RogueSquadron64Recomp/build/_deps/sdl2-subbuild/sdl2-populate-prefix"
  "E:/Projects/RogueSquadron64Recomp/build/_deps/sdl2-subbuild/sdl2-populate-prefix/tmp"
  "E:/Projects/RogueSquadron64Recomp/build/_deps/sdl2-subbuild/sdl2-populate-prefix/src/sdl2-populate-stamp"
  "E:/Projects/RogueSquadron64Recomp/build/_deps/sdl2-subbuild/sdl2-populate-prefix/src"
  "E:/Projects/RogueSquadron64Recomp/build/_deps/sdl2-subbuild/sdl2-populate-prefix/src/sdl2-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "E:/Projects/RogueSquadron64Recomp/build/_deps/sdl2-subbuild/sdl2-populate-prefix/src/sdl2-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "E:/Projects/RogueSquadron64Recomp/build/_deps/sdl2-subbuild/sdl2-populate-prefix/src/sdl2-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
