#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../common/xcat_sound.h"

#include <cstdio>
#include <cstring>

namespace {

void PrintUsage() {
    fputs("{\"ok\":false,\"cmd\":\"xcat_sound\",\"error\":\"usage\","
          "\"sounds\":[\"click\",\"confirm\",\"toggle\",\"error\",\"build-ok\",\"build-fail\","
          "\"success\",\"fail\",\"launch-ok\",\"launch-fail\",\"feature-ready\",\"notify\","
          "\"alarm\",\"alarm-timeout\",\"lie-pass\",\"lie-ok\",\"game-context\",\"gc-ok\"]}\n",
          stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    const char* name = argv[1];
    // "--async" 仅为兼容 build.bat 等历史调用方式而保留（直接跳过取真正的音效名）。
    // xcat_sound 是「播完一个音效就退出」的一次性进程：旧的 --async 会走 PlayNamed
    // 入队后立刻 Shutdown，而 Shutdown 会 queue.clear() + waveOutReset 打断当前播放，
    // 导致 build-ok / build-fail 被截断、甚至一声未响。独立进程里 async 没有意义
    // （build.bat 用 start /b 异步拉起时并不会阻塞构建），因此统一阻塞播放，
    // 保证音效完整播完后再退出。
    if (strcmp(name, "--async") == 0 && argc >= 3) {
        name = argv[2];
    }

    xcat::sound::Init();
    const bool ok = xcat::sound::PlayNamedBlocking(name);
    // WHDR_DONE 后 waveOut 已交完缓冲，但共享模式混音器/DAC 仍有几十 ms 残留；
    // 进程立刻退出会把尾部硬切断 → 爆音。与 UiShutdown 同套路：补短静音排空再关。
    if (ok) (void)xcat::sound::PlaySilenceBlocking(80);
    xcat::sound::Shutdown();

    if (!ok) {
        printf("{\"ok\":false,\"cmd\":\"xcat_sound\",\"sound\":\"%s\"}\n", name);
        return 1;
    }
    printf("{\"ok\":true,\"cmd\":\"xcat_sound\",\"sound\":\"%s\",\"blocking\":true}\n", name);
    return 0;
}
