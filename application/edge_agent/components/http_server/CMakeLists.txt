set(FRONTEND_SOURCE_DIR        "${CMAKE_CURRENT_LIST_DIR}/frontend_source")
set(FRONTEND_DIST_HTML_GZ      "${FRONTEND_SOURCE_DIR}/dist/index.html.gz")
set(FRONTEND_PUBLIC_FAVICON_ICO  "${FRONTEND_SOURCE_DIR}/public/favicon.ico")

if(NOT EXISTS "${FRONTEND_DIST_HTML_GZ}")
    message(FATAL_ERROR
        "\n"
        "Frontend bundle not found: ${FRONTEND_DIST_HTML_GZ}\n"
        "Build the SolidJS UI before running idf.py build, e.g.:\n"
        "    cd ${FRONTEND_SOURCE_DIR}\n"
        "    pnpm install --frozen-lockfile\n"
        "    pnpm run build\n")
endif()

set(http_server_srcs
    "http_server_core.c"
    "http_server_assets.c"
    "http_server_capabilities_api.c"
    "http_server_config_api.c"
    "http_server_lua_modules_api.c"
    "http_server_status_api.c"
    "http_server_files_api.c"
    "http_server_wechat_api.c"
    "http_server_webim_api.c"
    "http_server_hwtest_api.c"
    "http_server_json.c"
    "http_server_utils.c"
)

if(CONFIG_APP_CLAW_CAP_IM_VOICE)
    list(APPEND http_server_srcs "http_server_voice_api.c")
endif()

set(http_server_requires
    app_config
    app_claw
    cap_im_local
    cap_im_voice
    claw_cap
    esp_http_server
    esp_driver_i2c
    esp_driver_gpio
    esp_timer
    heap
)

if(CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER)
    list(APPEND http_server_srcs "http_server_lua_app_api.c")
    list(APPEND http_server_requires lua_module_http_server)
endif()

idf_component_register(
    SRCS
        ${http_server_srcs}
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        "${FRONTEND_DIST_HTML_GZ}"
        "${FRONTEND_PUBLIC_FAVICON_ICO}"
        "pages/voice_log.html"
        "pages/hwtest.html"
    REQUIRES
        ${http_server_requires}
)

# /hwtest camera capture uses the camera HAL (lua_module_camera) and the
# board-manager device handle (esp_board_manager). These only exist when the
# camera Lua module is enabled, and config-conditional REQUIRES do not work
# (requirements are resolved before CONFIG_* is available). optional_requires
# links them when present and is skipped otherwise — the source is config-gated.
idf_component_optional_requires(PRIVATE lua_module_camera esp_board_manager)

# Rebuild the component whenever the bundled UI asset changes on disk.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FRONTEND_DIST_HTML_GZ}")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
