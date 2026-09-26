# During component requirements scanning, CONFIG_ vars are not yet available.
# We must declare all possible dependencies upfront.
set(PRIV_REQUIRES driver nvs_flash esp_driver_i2c esp_driver_spi esp_adc main board_utils)

if(CONFIG_BOARD_TARGET_PATH)
    set(boards_dir ${CONFIG_BOARD_TARGET_PATH})
else()
    # Requirements scan phase - CONFIG_ vars are not available yet
    set(boards_dir "arply-v1")
endif()

idf_component_register(
    SRC_DIRS "${boards_dir}" "."
    INCLUDE_DIRS "${boards_dir}" "."
    REQUIRES esp_driver_i2c board_utils
    PRIV_REQUIRES ${PRIV_REQUIRES}
)
