// ExplosiveShotASI_raycast_with_sniperlist.cpp
// DK22Pac plugin-sdk (use Win32/x86). Rename DLL to ExplosiveShot.asi
#include "plugin.h"
#include "CVector.h"
#include "CWorld.h"
#include "CColPoint.h"
#include "CCamera.h"
#include "CExplosion.h"
#include "CPlayerPed.h"
#include "CEntity.h"

#include <windows.h>
#include <string>
#include <fstream>
#include <vector>
#include <sstream>

// Config
static const char* INI_FILENAME = "ExplosiveShot.ini";
static const int KEY_UNLOAD = VK_END;
static const int KEY_RELOAD_INI = VK_F5;
static const int FIRE_DEBOUNCE_MS = 150;
static const float RAYCAST_DISTANCE = 2000.0f; // distância máxima do raycast

// Estado
static HMODULE g_hModule = nullptr;
static bool g_running = true;
static bool g_enabled = true;
static int g_explosionType = 0;
static bool log_enabled = false;

// Novas configs
static std::vector<int> g_sniperWeapons; // lista de weapon ids que disparam explosão
static bool g_onlySniper = true;

// Debug log (útil se a detecção de arma falhar)
static void DebugLog(const char* fmt, ...) {
    if (log_enabled) {
        char path[MAX_PATH];
        GetModuleFileNameA(g_hModule ? g_hModule : GetModuleHandleA(NULL), path, MAX_PATH);
        std::string dir(path);
        size_t pos = dir.find_last_of("\\/");
        if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
        std::string full = dir + "ExplosiveShot_debug.txt";
        FILE* f = fopen(full.c_str(), "a");
        if (!f) return;
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        fprintf(f, "\n");
        va_end(ap);
        fclose(f);
    }
}

// Helpers INI
static std::string GetModuleDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_hModule ? g_hModule : GetModuleHandleA(NULL), path, MAX_PATH);
    std::string s(path);
    size_t pos = s.find_last_of("\\/");
    if (pos != std::string::npos) s = s.substr(0, pos + 1);
    return s;
}
static void CreateDefaultIniIfNotExists(const std::string& fullpath) {
    DWORD attrs = GetFileAttributesA(fullpath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return;
    std::ofstream out(fullpath);
    out << "[settings]\n";
    out << "enabled=1\n";
    out << "explosionType=0\n";
    out << "sniperWeapons=34\n";
    out << "onlySniper=1\n";
    out.close();
}
static void ParseSniperList(const std::string& s) {
    g_sniperWeapons.clear();
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, ',')) {
        // trim spaces
        size_t a = token.find_first_not_of(" \t\r\n");
        size_t b = token.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        token = token.substr(a, b - a + 1);
        try {
            int id = std::stoi(token);
            g_sniperWeapons.push_back(id);
        }
        catch (...) { /* ignore invalid */ }
    }
}

static void LoadConfig() {
    std::string dir = GetModuleDir();
    std::string full = dir + INI_FILENAME;
    CreateDefaultIniIfNotExists(full);
    g_enabled = GetPrivateProfileIntA("settings", "enabled", 1, full.c_str()) != 0;
    g_explosionType = GetPrivateProfileIntA("settings", "explosionType", 0, full.c_str());
    char buf[1024] = { 0 };
    GetPrivateProfileStringA("settings", "sniperWeapons", "34", buf, sizeof(buf), full.c_str());
    ParseSniperList(buf);
    g_onlySniper = GetPrivateProfileIntA("settings", "onlySniper", 1, full.c_str()) != 0;
    DebugLog("INI loaded: enabled=%d explosionType=%d onlySniper=%d sniperCount=%zu", g_enabled, g_explosionType, g_onlySniper, g_sniperWeapons.size());
}

// Cria explosão usando CExplosion wrapper do SDK
static void CreateExplosionAt(const CVector& pos, int explosionType) {
    CExplosion::AddExplosion(nullptr, nullptr, (eExplosionType)explosionType, pos, 0.0f, true, -1.0f, false);
}

// Pega posição/forward da câmera (TheCamera existe no SDK)
static CVector GetCameraPosition() {
    return TheCamera.GetPosition();
}
static CVector GetCameraForward() {
    return TheCamera.GetForward();
}

// Pega câmera ativa (mais confiável que GetPosition/GetForward)
static inline void GetActiveCam(CVector& outPos, CVector& outForward) {
    auto& cam = TheCamera.m_aCams[TheCamera.m_nActiveCam];
    outPos = cam.m_vecSource;   // posição da câmera
    outForward = cam.m_vecFront;    // direção da câmera
    // normaliza por segurança
    float mag = sqrtf(outForward.x * outForward.x + outForward.y * outForward.y + outForward.z * outForward.z);
    if (mag > 0.0001f) {
        outForward.x /= mag;
        outForward.y /= mag;
        outForward.z /= mag;
    }
    else {
        // fallback: olha pra frente no eixo Y
        outForward = CVector(0.0f, 1.0f, 0.0f);
    }
}

// Raycast ignorando o player
static bool DoRaycast(const CVector& start, const CVector& end, CColPoint& outCol, CEntity*& outEntity) {
    // ignore o player para não bater em si mesmo
    CEntity* oldIgnore = CWorld::pIgnoreEntity;
    CWorld::pIgnoreEntity = FindPlayerPed();

    bool hit = CWorld::ProcessLineOfSight(
        start, end, outCol, outEntity,
        true,   // buildings
        true,   // vehicles
        true,   // peds
        true,   // objects
        true,   // dummies
        true,   // doSeeThroughCheck
        false,  // doCameraIgnoreCheck
        true    // doShootThroughCheck
    );

    CWorld::pIgnoreEntity = oldIgnore; // restaura
    return hit;
}
int GetPlayerCurrentWeaponId() {
    CPlayerPed* player = FindPlayerPed();
    if (player) {
        CWeapon* activeWeapon = player->GetWeapon();
        if (activeWeapon) {
            return activeWeapon->m_eWeaponType; // Weapon ID do SDK
        }
    }
    return -1; // sem arma
}

// Verifica se id está na lista sniper
static bool IsWeaponInSniperList(int weaponId) {
    if (weaponId < 0) return false;
    for (int id : g_sniperWeapons) if (id == weaponId) return true;
    return false;
}

// Decide se explode (leva em conta onlySniper)
static bool ShouldExplodeForPlayer() {
    if (!g_onlySniper) return true; // explode para qualquer arma
    int wid = GetPlayerCurrentWeaponId();
    if (wid < 0) {
        // se não conseguimos detectar arma, log e considerar falso (mais seguro)
        DebugLog("ShouldExplodeForPlayer: couldn't detect weapon ID, refusing to explode");
        return false;
    }
    bool ok = IsWeaponInSniperList(wid);
    DebugLog("ShouldExplodeForPlayer: weaponId=%d allowed=%d", wid, ok ? 1 : 0);
    return ok;
}

// Thread principal
DWORD WINAPI MainThread(LPVOID) {
    LoadConfig();
    ULONGLONG lastFireTick = 0;

    while (g_running) {
        if (GetAsyncKeyState(KEY_UNLOAD) & 1) break;
        if (GetAsyncKeyState(KEY_RELOAD_INI) & 1) LoadConfig();

        if (g_enabled) {
            static int lastAmmoInClip = -1; // ammo da arma no frame anterior

            CPlayerPed* player = FindPlayerPed();
            if (player) {
                CWeapon* weapon = player->GetWeapon();
                if (weapon && ShouldExplodeForPlayer()) {
                    int currentAmmo = weapon->m_nAmmoInClip;

                    // Detecta disparo: quando o ammo diminui
                    if (lastAmmoInClip >= 0 && currentAmmo < lastAmmoInClip) {
                        // tiro detectado, cria explosão
                        CVector camPos, forward;
                        GetActiveCam(camPos, forward);
                        CVector end = camPos + forward * RAYCAST_DISTANCE;

                        CColPoint col;
                        CEntity* hitEntity = nullptr;
                        bool hit = DoRaycast(camPos, end, col, hitEntity);

                        CVector hitPos = hit ? col.m_vecPoint : end;
                        CreateExplosionAt(hitPos, g_explosionType);
                    }

                    lastAmmoInClip = currentAmmo; // atualiza ammo
                }
                else {
                    lastAmmoInClip = -1; // reset se não estiver com arma
                }
            }
        }


        Sleep(8);
    }

    if (g_hModule) FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_running = false;
    }
    return TRUE;
}
