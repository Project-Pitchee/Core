cmake_minimum_required(VERSION 3.20)
project(PitcheeCoreConsumer LANGUAGES C)

find_package(PitcheeCore CONFIG REQUIRED)

add_executable(pitchee_consumer main.c)
target_link_libraries(pitchee_consumer PRIVATE Pitchee::pitchee_core)
