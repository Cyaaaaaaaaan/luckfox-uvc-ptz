cmake_minimum_required(VERSION 3.5...3.25)
project(visca_server C)

add_executable(visca_server
    main.c
    visca.c
    motor.c
    isp_ctrl.c
)

target_compile_options(visca_server PRIVATE -Wall -Wextra)

install(TARGETS visca_server DESTINATION bin)
