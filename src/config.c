#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "frozen.h"
#include "config.h"

// initConfig
config_t* initConfig() {
    config_t* c;
    if ((c = malloc(sizeof *c)) != NULL) {
        c->configFile = "";
        c->watchDir = AM_DEFAULT_WATCH_DIR;
        c->workingDir = AM_DEFAULT_WORK_DIR;
        c->pid = -1;
        c->running = false;
        c->k_size = AM_DEFAULT_K_SIZE;
        c->sketch_size = AM_DEFAULT_SKETCH_SIZE;
        c->bloom_fp_rate = AM_DEFAULT_BLOOM_FP_RATE;
        c->bloom_max_elements = AM_DEFAULT_BLOOM_MAX_EL;
    }
    return c;
}

// destroyConfig
void destroyConfig(config_t* config) {
    free(config);
    config = NULL;
}

// writeConfig
int writeConfig(config_t* config, char* configFile) {

    // make sure the config is populated with something
    if (config == 0) return 1;

    // update with config with the filepath we are writing to
    config->configFile = configFile;

    // write it to file
    json_fprintf(configFile, "{ configFile: %Q, workingDirectory: %Q, watchDirectory: %Q, pid: %d, running: %B, k_size: %d, sketch_size: %d, bloom_fp_rate, %f, bloom_max_elements: %d }",
    config->configFile,
    config->workingDir,
    config->watchDir,
    config->pid,
    config->running,
    config->k_size,
    config->sketch_size,
    config->bloom_fp_rate,
    config->bloom_max_elements   
    );

    // prettify the json
    json_prettify_file(configFile);
    return 0;
}

// loadConfig
int loadConfig(config_t* config, char* configFile) {

    // create a stack allocated tmp config
    config_t c = { .pid = -1, .watchDir = NULL };

    // read the file into a buffer
    char* content = json_fread(configFile);

    // scan the file content and populate the tmp config
    int status = json_scanf(content, strlen(content), "{ configFile: %Q, watchDirectory: %Q, pid: %d, running: %B }", &c.configFile, &c.watchDir, &c.pid, &c.running);

    // check for error in json scan (-1 == error, 0 == no elements found, >0 == elements parsed)
    if (status < 1) {
        return 1;
    }

    // copy the config over to the heap
    config->configFile = c.configFile;
    config->watchDir = c.watchDir;
    config->pid = c.pid;
    config->running = c.running;
    return 0;
}
