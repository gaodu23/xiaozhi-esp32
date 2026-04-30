idf_component_register(
    SRCS "RNetController.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver freertos esp_timer
)
