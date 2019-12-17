#ifndef CONFIG_H
#define CONFIG_H

#define AM_PROG_NAME "antman"
#define AM_DEFAULT_K_SIZE 7
#define AM_DEFAULT_SKETCH_SIZE 128
#define AM_DEFAULT_BLOOM_FP_RATE 0.001
#define AM_DEFAULT_BLOOM_MAX_EL 100000

/*
    config_t is used to record the minimum information required by antman
*/
typedef struct config
{
    char *configFile;
    char *logFile;
    char *watchDir;
    int pid;
    int k_size;
    int sketch_size;
    double bloom_fp_rate;
    int bloom_max_elements;
} config_t;

/*
    function prototypes
*/
config_t *initConfig();
void destroyConfig(config_t *config);
int writeConfig(config_t *config, char *configFile);
int loadConfig(config_t *config, char *configFile);

#endif