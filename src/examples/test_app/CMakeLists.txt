px4_add_module(
    MODULE examples_test_app
    MAIN test_app
    COMPILE_FLAGS
    SRCS
       test_app_main.cpp
    DEPENDS
)
