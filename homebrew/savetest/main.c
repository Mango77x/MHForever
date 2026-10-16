// savetest: sceUtility savedata round trips through the runtime (docs/05-runtime-hle.md, savedata).
// Our own code; GPL-2.0-only like the rest of the repository.
#include <pspkernel.h>
#include <psputility.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("savetest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

// Request modes (numbering as in psputility_savedata.h; the SDK's own enum names differ between versions).
enum { MODE_AUTOLOAD = 0, MODE_AUTOSAVE = 1, MODE_LISTLOAD = 4, MODE_LISTSAVE = 5, MODE_SIZES = 8, MODE_DELETE = 10,
       MODE_LIST = 11 };

static int passed, failed;

static void check(const char *name, int got, int expected) {
    if (got == expected) { ++passed; } else { ++failed; printf("FAIL %s: got %d (0x%08x) expected %d (0x%08x)\n", name, got, got, expected, expected); }
}

static char data[256], buffer[256];
static char slots[4][20] = {"SLOT0", "SLOT1", "SLOT2", ""};
static SceUtilitySavedataMsFreeInfo ms_free;
static SceUtilitySavedataMsDataInfo ms_data;
static SceUtilitySavedataUsedDataInfo utility_data;
static SceUtilitySavedataIdListEntry ids[8];
static SceUtilitySavedataIdListInfo id_list;

// Runs one request the way games do: InitStart, then Update/GetStatus until finished, then Shutdown.
static int run(SceUtilitySavedataParam *p) {
    p->base.size = sizeof(*p);
    if (sceUtilitySavedataInitStart(p) < 0) return -1;
    for (int i = 0; i < 100; ++i) {
        int status = sceUtilitySavedataGetStatus();
        if (status == 3) {
            sceUtilitySavedataShutdownStart();
        } else if (status == 0) {
            return p->base.result;
        }
        sceUtilitySavedataUpdate(1);
        sceKernelDelayThread(1000);
    }
    return -2;
}

static void setup(SceUtilitySavedataParam *p, int mode, const char *save) {
    memset(p, 0, sizeof(*p));
    p->mode = mode;
    strcpy(p->gameName, "MHFTEST00");
    strcpy(p->saveName, save);
    strcpy(p->fileName, "DATA.BIN");
    p->dataBuf = buffer;
    p->dataBufSize = sizeof(buffer);
}

int main(void) {
    static SceUtilitySavedataParam p;
    for (int i = 0; i < (int)sizeof(data); ++i) data[i] = (char)(i * 7 + 3);

    // Start clean (the memstick folder persists between test runs).
    for (int i = 0; i < 3; ++i) { setup(&p, MODE_DELETE, slots[i]); run(&p); }

    setup(&p, MODE_AUTOLOAD, "SLOT0");
    check("load missing", run(&p), (int)0x80110307);

    setup(&p, MODE_SIZES, "SLOT0");
    strcpy(ms_data.gameName, "MHFTEST00");
    strcpy(ms_data.saveName, "SLOT0");
    p.msFree = &ms_free; p.msData = &ms_data; p.utilityData = &utility_data;
    p.dataSize = 100;
    check("sizes missing", run(&p), (int)0x801103C7);
    check("sizes cluster", ms_free.clusterSize, 0x8000);
    check("sizes needed", utility_data.usedClusters > 0, 1);

    setup(&p, MODE_AUTOSAVE, "SLOT0");
    memcpy(buffer, data, sizeof(data));
    p.dataSize = 200;
    strcpy(p.sfoParam.title, "Test title");
    strcpy(p.sfoParam.savedataTitle, "Save title");
    strcpy(p.sfoParam.detail, "Some details");
    check("save", run(&p), 0);

    setup(&p, MODE_AUTOLOAD, "SLOT0");
    memset(buffer, 0, sizeof(buffer));
    check("load", run(&p), 0);
    check("load size", p.dataSize, 200);
    check("load data", memcmp(buffer, data, 200), 0);
    check("load title", strcmp(p.sfoParam.title, "Test title"), 0);
    check("load save title", strcmp(p.sfoParam.savedataTitle, "Save title"), 0);
    check("load detail", strcmp(p.sfoParam.detail, "Some details"), 0);

    setup(&p, MODE_SIZES, "SLOT0");
    p.msData = &ms_data;
    check("sizes present", run(&p), 0);
    check("sizes used", ms_data.info.usedClusters > 0, 1);

    // List save with "first empty" focus picks SLOT1 and reports it back.
    setup(&p, MODE_LISTSAVE, "");
    p.saveNameList = slots;
    p.focus = PSP_UTILITY_SAVEDATA_FOCUS_FIRSTEMPTY;
    memcpy(buffer, data + 1, 50);
    p.dataSize = 50;
    check("listsave", run(&p), 0);
    check("listsave slot", strcmp(p.saveName, "SLOT1"), 0);

    // List load with "latest" focus picks the most recent existing save.
    setup(&p, MODE_LISTLOAD, "");
    p.saveNameList = slots;
    p.focus = PSP_UTILITY_SAVEDATA_FOCUS_LATEST;
    check("listload", run(&p), 0);
    check("listload found", strcmp(p.saveName, "SLOT0") == 0 || strcmp(p.saveName, "SLOT1") == 0, 1);

    setup(&p, MODE_LIST, "<>");
    id_list.maxCount = 8; id_list.entries = ids;
    p.idList = &id_list;
    check("list", run(&p), 0);
    check("list count", id_list.resultCount, 2);
    check("list first", strcmp(ids[0].name, "SLOT0"), 0);

    setup(&p, MODE_DELETE, "SLOT1");
    check("delete", run(&p), 0);
    setup(&p, MODE_AUTOLOAD, "SLOT1");
    check("load deleted", run(&p), (int)0x80110307);

    printf("savetest: %d passed, %d failed\n", passed, failed);
    sceKernelExitGame();
    return 0;
}
