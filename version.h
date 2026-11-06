/* ============================================================================
 *  version.h  —  固件版本检查
 * ----------------------------------------------------------------------------
 *  版本命名规则 v<MAJOR>.<MINOR>.<PATCH>+<构建号>
 *    MAJOR 主版本  改动破坏兼容性  老设备/老客户端/老网关升级后可能连不上或功能失效
 *    MINOR 次版本  新增向下兼容的功能  老版本仍能用，新版本多出能力
 *    PATCH 修订号  不新增功能的修复  只修 bug / 性能 / 安全，对外行为不变
 *
 *  本版本号 v1.3.4：增加json配置功能方便更改配置。
 *
 *  用法：
 *      #include "version.h"
 *      fwver::check();          // 手动调用一次
 *      或串口敲 debug update    （fan_main.ino 里已接好）
 *
 *  ★ 前提：WiFi 必须已连上。检查前先判 net::wifiOk()。
 *  ★ version.txt 里只放一行版本号（如 v1.3.2），不要注释/空行/说明文字。
 * ============================================================================
 */
#ifndef VERSION_H
#define VERSION_H

#include <Arduino.h>                /* Serial / F() —— 不靠 WiFi.h 间接带进来 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>

/* 下面这行是「自包含」用的，别删：
   check() 里要调 thermal::reloadModel()，不自己 include 就得靠 fan_main.ino 的包含顺序
   （thermal_sensor.h 必须在 version.h 前面），顺序一改就编译不过。
   现在没有循环包含：thermal_sensor.h → fan_config/fan_types/model.h，这条链上没人 include version.h。
   哪天真出现循环，也是那一步要先拆，而不是从这里删。 */
#include "thermal_sensor.h"

namespace fwver {

/* 当前固件版本 —— 改版本号只改这里 */
static constexpr const char *kVersion = "v1.3.4";

/* 版本文件地址 */
static constexpr const char *kUrl = "https://xuzeng2.github.io/web/version.txt";
/* 模型文件地址（debug model update / check() 检测到新版时下载用）*/
static constexpr const char *kDownload = "https://xuzeng2.github.io/web/amg_model_int8.tflite";
/* ---------------------------------------------------------------------------
 * 检查版本
 *   返回 true  = 已是最新
 *   返回 false = 有新版 / 网络失败 / 服务器返回空
 *
 *  ★ 阻塞调用：内部走 HTTPClient::GET()，WiFi 差时可能卡 1~2 秒。
 *    只在 WiFi 已连接时调用（先判 net::wifiOk()），否则会卡在连接超时上。
 *  ★ HTTPS 用 WiFiClientSecure + setInsecure() 跳过证书校验，
 *    只适合版本号这种低风险场景；要严格校验就换成 setCACert()。
 * ------------------------------------------------------------------------- */

/**
 * 从指定 URL 下载文件并替换 LittleFS 中的目标文件
 * 
 * @param url       下载地址（支持 http 和 https）
 * @param savePath  保存到 LittleFS 的路径，例如 "/amg_model_int8.tflite"
 * @return          true = 下载并替换成功；false = 失败（原文件保持不变）
 */
static inline bool downloadAndReplaceFile(const String& url, const String& savePath) 
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[下载] WiFi 未连接");
        return false;
    }

    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure(); // 跳过 SSL 证书校验

    http.begin(client, url);
    http.setTimeout(15000); // 15秒超时

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[下载] HTTP 失败, code=%d\n", httpCode);
        http.end();
        return false;
    }

    int contentLen = http.getSize();
    Serial.printf("[下载] 开始，大小: %d 字节\n", contentLen);

    // 1. 先下载到临时文件
    String tmpPath = savePath + ".tmp";
    File file = LittleFS.open(tmpPath, "w");
    if (!file) {
        Serial.println("[下载] 无法创建临时文件");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[1024];
    int written = 0;
    bool success = true;
    uint32_t t0 = millis();

    while (http.connected() && (contentLen > 0 || contentLen == -1)) {
        size_t available = stream->available();
        if (available) {
            int read = (int)stream->readBytes(buffer, min(available, sizeof(buffer)));
            if (read <= 0) break;              /* 读不出来了，别空转 */
            file.write(buffer, (size_t)read);
            written += read;
            if (contentLen > 0) contentLen -= read;
            t0 = millis();
        } else {
            /* 对端不发了也不关连接 → 不能死等，否则整个 loop 卡在这儿 */
            if (millis() - t0 > 20000) {
                Serial.println("[下载] 超时（20 秒没收到数据），放弃");
                success = false;
                break;
            }
            delay(1); // 让出 CPU，防看门狗
        }
    }

    file.close();
    http.end();

    /* 对端提前断连：connected() 变 false 会让循环正常退出，
       但 contentLen 还剩一截 —— 那就是个下了一半的文件，不能拿去替换。 */
    if (success && contentLen > 0) {
        Serial.printf("[下载] 不完整，还差 %d 字节\n", contentLen);
        success = false;
    }

    if (!success) {
        LittleFS.remove(tmpPath);
        return false;
    }

    // 2. 检查下载是否完整（简单判定：大于 1KB 且不是负数）
    if (written < 1024) {
        Serial.printf("[下载] 文件太小 (%d 字节)，取消替换\n", written);
        LittleFS.remove(tmpPath);
        return false;
    }

    // 3. 替换原文件
    if (LittleFS.exists(savePath)) {
        LittleFS.remove(savePath);
    }
    if (!LittleFS.rename(tmpPath, savePath)) {
        Serial.println("[下载] 重命名失败");
        return false;
    }

    Serial.printf("[下载] 成功！已保存到 %s (%d 字节)\n", savePath.c_str(), written);
    return true;
}
static inline bool check()
{
    WiFiClientSecure client;
    client.setInsecure();               /* 跳过证书校验（测试/低风险场景用） */

    HTTPClient http;
    http.begin(client, kUrl);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[跟新] 请求失败, code=%d\n", httpCode);
        http.end();
        return false;                   /* 失败直接返回，不比较 */
    }

    String latest = http.getString();
    latest.trim();                      /* 去掉行尾 \n / \r\n */
    http.end();                         /* 只在这里 end 一次 */

    if (latest.length() == 0) {
        Serial.println(F("[跟新] 服务器返回空内容"));
        return false;
    }

    if (latest == kVersion) {
        Serial.printf("[跟新] 已是最新: %s\n", kVersion);
        return true;
    }

    Serial.printf("[跟新] 有新版！当前 %s → 最新 %s\n",kVersion, latest.c_str());
    if (downloadAndReplaceFile(kDownload, "/amg_model_int8.tflite")) {
        Serial.println("[跟新] 新模型替换成功，正在重新加载模型");
        thermal::reloadModel();
    }
    return false;
}

/* 当前版本号（字符串），串口/网页显示用 */
static inline const char *version() { return kVersion; }

} /* namespace fwver */

#endif /* VERSION_H */