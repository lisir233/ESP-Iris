#pragma once
typedef struct { char project_name[33]; char version[33]; char idf_ver[33]; unsigned char app_elf_sha256[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
