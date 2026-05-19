Vendored cJSON. Source: https://github.com/DaveGamble/cJSON (tag v1.7.18, MIT).

NOTE: the scaffold environment had no network, so cJSON.c / cJSON.h here are
minimal stubs. Replace with the real upstream files before parsing any JSON:

    curl -sSL -o src/cjson/cJSON.c https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c
    curl -sSL -o src/cjson/cJSON.h https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h
