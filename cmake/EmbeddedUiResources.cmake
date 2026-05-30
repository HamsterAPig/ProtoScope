include_guard(GLOBAL)

set(PROTOSCOPE_UI_RESOURCE_FILES
    "${PROJECT_SOURCE_DIR}/assets/fonts/fa-solid-900.ttf"
)

cmrc_add_resource_library(ui_resources
    NAMESPACE ui_resources
    WHENCE "${PROJECT_SOURCE_DIR}"
    ${PROTOSCOPE_UI_RESOURCE_FILES}
)
