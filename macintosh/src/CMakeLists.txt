cmake_minimum_required(VERSION 3.13)
project(DesignerForm C)

add_application(DesignerForm
    main.c
    designer_form.c
    designer_form.r
    user_events.c
)

set_target_properties(DesignerForm PROPERTIES
    COMPILE_OPTIONS -ffunction-sections
    LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-single"
)