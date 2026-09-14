#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "test.h"
#include "utils.h"

#define TEST_PATH "/tmp/max-agent-test-config.json"

static int build_test_config(Config *cfg)
{
    memset(cfg, 0, sizeof *cfg);

    cfg->providers = calloc(1, sizeof(Provider));
    if (!cfg->providers) {
        return -1;
    }
    cfg->providers_len = 1;

    Provider *p = &cfg->providers[0];
    p->api = OPENAI_COMPLETIONS;
    p->api_key = dup_str("sk-test-123");
    p->base_url = dup_str("https://api.example.com/v1");
    if (!p->api_key || !p->base_url) {
        return -1;
    }

    p->models = calloc(2, sizeof(Model));
    if (!p->models) {
        return -1;
    }
    p->models_len = 2;

    Model *m0 = &p->models[0];
    m0->id = dup_str("gpt-test");
    m0->id_len = strlen("gpt-test");
    m0->context_window = 200000;
    m0->reasoning = true;
    m0->inputTypes = calloc(2, sizeof(InputType));
    if (!m0->id || !m0->inputTypes) {
        return -1;
    }
    m0->inputTypes[0] = TEXT;
    m0->inputTypes[1] = IMAGE;
    m0->input_types_len = 2;

    Model *m1 = &p->models[1];
    m1->id = dup_str("mini-test");
    m1->id_len = strlen("mini-test");
    m1->context_window = 128000;
    m1->reasoning = false;

    return 0;
}

int main(void)
{
    /* --- roundtrip: bauen -> speichern -> laden -> vergleichen --- */
    Config cfg;
    CHECK(build_test_config(&cfg) == 0);
    CHECK(save_config_to(TEST_PATH, &cfg) == 0);

    Config loaded;
    CHECK(load_config_from(TEST_PATH, &loaded) == 0);
    CHECK(loaded.providers_len == 1);

    Provider *p = &loaded.providers[0];
    CHECK(p->api == OPENAI_COMPLETIONS);
    CHECK(p->api_key != NULL && strcmp(p->api_key, "sk-test-123") == 0);
    CHECK(p->base_url != NULL &&
          strcmp(p->base_url, "https://api.example.com/v1") == 0);
    CHECK(p->models_len == 2);

    Model *m0 = &p->models[0];
    CHECK(m0->id != NULL && strcmp(m0->id, "gpt-test") == 0);
    CHECK(m0->id_len == strlen("gpt-test"));
    CHECK(m0->context_window == 200000);
    CHECK(m0->reasoning);
    CHECK(m0->input_types_len == 2);
    CHECK(m0->inputTypes[0] == TEXT);
    CHECK(m0->inputTypes[1] == IMAGE);

    Model *m1 = &p->models[1];
    CHECK(m1->id != NULL && strcmp(m1->id, "mini-test") == 0);
    CHECK(m1->context_window == 128000);
    CHECK(!m1->reasoning);
    CHECK(m1->input_types_len == 0);
    CHECK(m1->inputTypes == NULL);

    free_config(&cfg);
    free_config(&loaded);

    /* --- fehlende datei -> leere config, kein fehler --- */
    unlink(TEST_PATH);
    Config empty;
    CHECK(load_config_from(TEST_PATH, &empty) == 0);
    CHECK(empty.providers == NULL);
    CHECK(empty.providers_len == 0);
    free_config(&empty);

    /* --- kaputte datei -> fehler --- */
    CHECK(write_file(TEST_PATH, "kein json", strlen("kein json")) == 0);
    Config broken;
    CHECK(load_config_from(TEST_PATH, &broken) == -1);
    CHECK(broken.providers == NULL);
    CHECK(broken.providers_len == 0);
    free_config(&broken);
    unlink(TEST_PATH);

    return test_report();
}