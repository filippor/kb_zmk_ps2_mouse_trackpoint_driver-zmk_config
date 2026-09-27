cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT /workspaces/zmk/app/module)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(oled_test)

target_sources(app PRIVATE src/main.c)
