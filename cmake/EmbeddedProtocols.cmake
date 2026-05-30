include_guard(GLOBAL)

# 默认协议脚本以资源形式嵌入，运行时可用于首次初始化协议目录。
file(GLOB_RECURSE PROTOSCOPE_PROTOCOL_RESOURCE_FILES
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/protocols/*"
)

set(PROTOSCOPE_EMBEDDED_PROTOCOL_FILE_INITIALIZERS "")
set(PROTOSCOPE_EMBEDDED_PROTOCOL_FILE_COUNT 0)

foreach(resource_file IN LISTS PROTOSCOPE_PROTOCOL_RESOURCE_FILES)
    if(IS_DIRECTORY "${resource_file}")
        continue()
    endif()

    file(RELATIVE_PATH resource_path "${PROJECT_SOURCE_DIR}" "${resource_file}")
    string(REPLACE "\\" "/" resource_path "${resource_path}")

    file(RELATIVE_PATH output_path "${PROJECT_SOURCE_DIR}/protocols" "${resource_file}")
    string(REPLACE "\\" "/" output_path "${output_path}")

    # 运行时 protocols 根目录保留 LuaLS 注解和 API 说明；协议示例统一释放到 templates。
    if(output_path MATCHES "^[^/]+/.+" AND NOT output_path MATCHES "^templates/")
        set(output_path "templates/${output_path}")
    endif()

    string(APPEND PROTOSCOPE_EMBEDDED_PROTOCOL_FILE_INITIALIZERS
        "    {\"${resource_path}\", \"${output_path}\"},\n"
    )

    math(EXPR PROTOSCOPE_EMBEDDED_PROTOCOL_FILE_COUNT "${PROTOSCOPE_EMBEDDED_PROTOCOL_FILE_COUNT} + 1")
endforeach()

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/embedded_protocols_manifest.hpp.in"
    "${PROJECT_BINARY_DIR}/generated/protoscope/config/embedded_protocols_manifest.hpp"
    @ONLY
)

cmrc_add_resource_library(proto_resources
    NAMESPACE proto_resources
    WHENCE "${PROJECT_SOURCE_DIR}"
    ${PROTOSCOPE_PROTOCOL_RESOURCE_FILES}
)
