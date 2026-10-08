if(NOT DEFINED PROTOSCOPE_RUNTIME_DIR OR NOT DEFINED PROTOSCOPE_SOURCE_DIR)
    message(FATAL_ERROR "复制运行时配置需要 PROTOSCOPE_RUNTIME_DIR 和 PROTOSCOPE_SOURCE_DIR")
endif()

set(runtime_config_dir "${PROTOSCOPE_RUNTIME_DIR}/config")
set(runtime_theme_dir "${runtime_config_dir}/themes")
file(MAKE_DIRECTORY "${runtime_theme_dir}")

set(runtime_files
    "protoscope.yaml"
)

foreach(relative_path IN LISTS runtime_files)
    set(source_path "${PROTOSCOPE_SOURCE_DIR}/config/${relative_path}")
    set(destination_path "${runtime_config_dir}/${relative_path}")
    if(NOT EXISTS "${destination_path}")
        configure_file("${source_path}" "${destination_path}" COPYONLY)
    endif()
endforeach()
