#######################
#
#  Licensed to the Apache Software Foundation (ASF) under one or more contributor license
#  agreements.  See the NOTICE file distributed with this work for additional information regarding
#  copyright ownership.  The ASF licenses this file to you under the Apache License, Version 2.0
#  (the "License"); you may not use this file except in compliance with the License.  You may obtain
#  a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software distributed under the License
#  is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
#  or implied. See the License for the specific language governing permissions and limitations under
#  the License.
#
#######################

# Findlibswoc.cmake
#
# This will define the following variables
#
#     libswoc_FOUND
#     libswoc_LIBRARY
#     libswoc_INCLUDE_DIRS
#
# and the following imported targets
#
#     libswoc::libswoc
#

find_package(PkgConfig REQUIRED)
pkg_check_modules(libswoc libswoc)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  libswoc
  REQUIRED_VARS libswoc_INCLUDE_DIRS libswoc_LINK_LIBRARIES libswoc_LIBRARIES libswoc_LIBRARY_DIRS
  HANDLE_COMPONENTS
)

if(libswoc_FOUND)
  set(libswoc_INCLUDE_DIRS ${libswoc_INCLUDE_DIR})
endif()

if(libswoc_FOUND AND NOT TARGET libswoc::libswoc)
  add_library(libswoc::libswoc INTERFACE IMPORTED)
  target_include_directories(libswoc::libswoc INTERFACE ${libswoc_INCLUDE_DIRS})
  target_link_directories(libswoc::libswoc INTERFACE ${libswoc_LIBRARY_DIRS})
  target_link_libraries(libswoc::libswoc INTERFACE ${libswoc_LIBRARIES})
endif()
