cmake_minimum_required(VERSION 3.20)

project(wx_combo_alignment_test LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wxWidgets REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

add_executable(wx-combo-alignment-test MACOSX_BUNDLE main.mm)
target_link_libraries(wx-combo-alignment-test PRIVATE ${wxWidgets_LIBRARIES} "-framework AppKit")
