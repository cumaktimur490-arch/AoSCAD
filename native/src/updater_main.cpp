#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <urlmon.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <objbase.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <functional>
#include <iterator>
#include <atomic>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef AOSCAD_VERSION
#define AOSCAD_VERSION "0.3.0"
#endif
#define AOSCAD_WIDEN_IMPL(value) L##value
#define AOSCAD_WIDEN(value) AOSCAD_WIDEN_IMPL(value)

namespace {
constexpr wchar_t kWindowClass[] = L"AoSCADStudioUpdaterWindow";
constexpr wchar_t kUpdateInfoUrl[] = L"https://github.com/cumaktimur490-arch/AoSCAD/releases/latest/download/AoSCAD-Studio-UpdateInfo.json";
constexpr wchar_t kLatestInstallerUrl[] = L"https://github.com/cumaktimur490-arch/AoSCAD/releases/latest/download/AoSCAD-Studio-Setup-Latest.exe";
constexpr wchar_t kCurrentVersion[] = AOSCAD_WIDEN(AOSCAD_VERSION);
constexpr UINT kMessageStatus = WM_APP + 1;
constexpr UINT kMessageProgress = WM_APP + 2;
constexpr UINT kMessageCheckComplete = WM_APP + 3;
constexpr UINT kMessageDownloadComplete = WM_APP + 4;
constexpr UINT kMessageInstallComplete = WM_APP + 5;
constexpr UINT kMessageInstalling = WM_APP + 6;
constexpr int kPrimaryButtonId = 101;
constexpr int kCancelButtonId = 102;

enum class Stage {
    Checking,
    ReadyToDownload,
    Downloading,
    Downloaded,
    Installing,
    Finished
};

struct WorkerResult {
    bool success{false};
    bool cancelled{false};
    bool updateAvailable{false};
    std::wstring version;
    std::wstring sha256;
    std::filesystem::path installerPath;
    std::wstring message;
};

struct AppState {
    HWND window{nullptr};
    HWND statusLabel{nullptr};
    HWND progressBar{nullptr};
    HWND primaryButton{nullptr};
    HWND cancelButton{nullptr};
    std::thread worker;
    std::atomic_bool cancelRequested{false};
    DWORD parentPid{0};
    bool workerRunning{false};
    std::atomic_bool installationStarted{false};
    Stage stage{Stage::Checking};
    std::wstring latestVersion;
    std::wstring expectedSha256;
    std::filesystem::path installDirectory;
    std::filesystem::path installerPath;
};

struct Arguments {
    DWORD parentPid{0};
    bool workerCopy{false};
    std::filesystem::path installDirectory;
};

std::wstring modulePath()
{
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    return std::wstring(buffer.data(), length);
}

std::wstring directoryOf(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::wstring quoteArgument(const std::wstring& argument)
{
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(ch);
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(ch);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

Arguments parseArguments()
{
    Arguments result;
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return result;
    for (int i = 1; i < count; ++i) {
        const std::wstring current = arguments[i];
        if (current == L"--worker") {
            result.workerCopy = true;
        } else if (current == L"--parent-pid" && i + 1 < count) {
            try {
                result.parentPid = static_cast<DWORD>(std::stoul(arguments[++i]));
            } catch (...) {
                result.parentPid = 0;
            }
        } else if (current == L"--install-dir" && i + 1 < count) {
            result.installDirectory = arguments[++i];
        }
    }
    LocalFree(arguments);
    return result;
}

bool relaunchFromTemporaryDirectory(const Arguments& arguments, const std::wstring& sourcePath)
{
    if (arguments.workerCopy)
        return true;

    std::wstring installDirectory = arguments.installDirectory.empty()
        ? directoryOf(sourcePath)
        : arguments.installDirectory.wstring();
    if (installDirectory.empty())
        return false;

    wchar_t tempDirectory[MAX_PATH + 1]{};
    const DWORD tempLength = GetTempPathW(MAX_PATH, tempDirectory);
    if (tempLength == 0 || tempLength >= MAX_PATH)
        return false;

    const std::wstring tempPath = std::wstring(tempDirectory) + L"AoSCAD-Updater-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".exe";
    if (!CopyFileW(sourcePath.c_str(), tempPath.c_str(), FALSE))
        return false;

    std::wstring commandLine = quoteArgument(tempPath) + L" --worker --install-dir " + quoteArgument(installDirectory);
    if (arguments.parentPid != 0)
        commandLine += L" --parent-pid " + std::to_wstring(arguments.parentPid);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(tempPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, installDirectory.c_str(), &startupInfo, &processInfo)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

std::wstring hresultText(HRESULT result)
{
    std::wostringstream text;
    text << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return text.str();
}

class DownloadCallback final : public IBindStatusCallback {
public:
    DownloadCallback(HWND window, std::atomic_bool& cancelRequested)
        : window_(window), cancelRequested_(cancelRequested)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IBindStatusCallback) {
            *object = static_cast<IBindStatusCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnProgress(ULONG progress, ULONG progressMax, ULONG, LPCWSTR) override
    {
        if (cancelRequested_.load())
            return E_ABORT;
        if (window_ && progressMax > 0) {
            const WPARAM percentage = static_cast<WPARAM>(std::min<ULONGLONG>(100,
                static_cast<ULONGLONG>(progress) * 100 / progressMax));
            PostMessageW(window_, kMessageProgress, percentage, 1);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD* flags, BINDINFO* bindInfo) override
    {
        if (!flags || !bindInfo)
            return E_INVALIDARG;
        ZeroMemory(bindInfo, sizeof(BINDINFO));
        *flags = BINDF_GETNEWESTVERSION | BINDF_PRAGMA_NO_CACHE | BINDF_PULLDATA;
        bindInfo->cbSize = sizeof(BINDINFO);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }

private:
    std::atomic<ULONG> references_{1};
    HWND window_{nullptr};
    std::atomic_bool& cancelRequested_;
};

HRESULT downloadFile(const std::wstring& url,
                     const std::filesystem::path& destination,
                     HWND window,
                     std::atomic_bool& cancelRequested)
{
    if (cancelRequested.load())
        return E_ABORT;
    auto* callback = new DownloadCallback(window, cancelRequested);
    const HRESULT result = URLDownloadToFileW(nullptr, url.c_str(), destination.c_str(), 0, callback);
    callback->Release();
    if (cancelRequested.load())
        return E_ABORT;
    return result;
}

std::wstring utf8ToWide(const std::string& text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), length);
    return result;
}

bool readTextFile(const std::filesystem::path& path, std::string& result)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    result.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

bool jsonStringValue(const std::string& json, const std::string& key, std::string& value)
{
    const std::string marker = "\"" + key + "\"";
    const std::size_t keyPosition = json.find(marker);
    if (keyPosition == std::string::npos)
        return false;
    const std::size_t colon = json.find(':', keyPosition + marker.size());
    if (colon == std::string::npos)
        return false;
    std::size_t position = colon + 1;
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])))
        ++position;
    if (position >= json.size() || json[position] != '"')
        return false;
    ++position;
    value.clear();
    while (position < json.size()) {
        const char ch = json[position++];
        if (ch == '"')
            return true;
        if (ch == '\\') {
            if (position >= json.size())
                return false;
            const char escaped = json[position++];
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: return false;
            }
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

std::vector<unsigned long long> parseVersion(const std::wstring& version)
{
    std::wstring value = version;
    if (!value.empty() && (value.front() == L'v' || value.front() == L'V'))
        value.erase(value.begin());
    if (value.empty())
        return {};
    std::vector<unsigned long long> components;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(L'.', start);
        const std::wstring component = value.substr(start, end == std::wstring::npos ? end : end - start);
        if (component.empty() || !std::all_of(component.begin(), component.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
            return {};
        try {
            components.push_back(std::stoull(component));
        } catch (...) {
            return {};
        }
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }
    return components;
}

bool versionIsNewer(const std::wstring& candidate, const std::wstring& current)
{
    const auto candidateParts = parseVersion(candidate);
    const auto currentParts = parseVersion(current);
    if (candidateParts.empty() || currentParts.empty())
        return false;
    const std::size_t componentCount = std::max(candidateParts.size(), currentParts.size());
    for (std::size_t i = 0; i < componentCount; ++i) {
        const unsigned long long candidateValue = i < candidateParts.size() ? candidateParts[i] : 0;
        const unsigned long long currentValue = i < currentParts.size() ? currentParts[i] : 0;
        if (candidateValue != currentValue)
            return candidateValue > currentValue;
    }
    return false;
}

bool validSha256(const std::wstring& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](wchar_t ch) {
        return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F');
    });
}

bool calculateSha256(const std::filesystem::path& path, std::wstring& digest)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD resultLength = 0;
    std::vector<UCHAR> hashObject;
    std::vector<UCHAR> hashBytes;
    bool success = false;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &resultLength, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
                          sizeof(hashLength), &resultLength, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    hashObject.resize(objectLength);
    hashBytes.resize(hashLength);
    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (input) {
        std::array<char, 64 * 1024> buffer{};
        bool readOk = true;
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                            static_cast<ULONG>(count), 0) < 0) {
                readOk = false;
                break;
            }
        }
        if (readOk && input.eof() && BCryptFinishHash(hash, hashBytes.data(), hashLength, 0) >= 0) {
            static constexpr wchar_t hex[] = L"0123456789abcdef";
            digest.clear();
            digest.reserve(hashBytes.size() * 2);
            for (UCHAR byte : hashBytes) {
                digest.push_back(hex[(byte >> 4) & 0x0f]);
                digest.push_back(hex[byte & 0x0f]);
            }
            success = true;
        }
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

std::wstring versionFromExecutable(const std::filesystem::path& path)
{
    DWORD ignored = 0;
    const DWORD dataSize = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (dataSize == 0)
        return {};
    std::vector<BYTE> data(dataSize);
    if (!GetFileVersionInfoW(path.c_str(), 0, dataSize, data.data()))
        return {};

    VS_FIXEDFILEINFO* fileInfo = nullptr;
    UINT fileInfoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&fileInfo), &fileInfoSize) ||
        !fileInfo || fileInfoSize < sizeof(VS_FIXEDFILEINFO) || fileInfo->dwSignature != VS_FFI_SIGNATURE)
        return {};

    std::wostringstream version;
    version << HIWORD(fileInfo->dwFileVersionMS) << L'.'
            << LOWORD(fileInfo->dwFileVersionMS) << L'.'
            << HIWORD(fileInfo->dwFileVersionLS) << L'.'
            << LOWORD(fileInfo->dwFileVersionLS);
    return version.str();
}

void postStatus(AppState& state, const std::wstring& message)
{
    if (!state.window)
        return;
    auto* copy = new std::wstring(message);
    if (!PostMessageW(state.window, kMessageStatus, 0, reinterpret_cast<LPARAM>(copy)))
        delete copy;
}

void postWorkerResult(AppState& state, UINT message, WorkerResult result)
{
    if (!state.window)
        return;
    auto* copy = new WorkerResult(std::move(result));
    if (!PostMessageW(state.window, message, 0, reinterpret_cast<LPARAM>(copy)))
        delete copy;
}

void checkForUpdates(AppState& state)
{
    WorkerResult result;
    result.success = false;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        result.message = L"Не удалось инициализировать сетевой загрузчик.";
        postWorkerResult(state, kMessageCheckComplete, std::move(result));
        return;
    }

    postStatus(state, L"Проверка последнего релиза GitHub…");
    std::filesystem::path manifestPath;
    try {
        manifestPath = std::filesystem::temp_directory_path() /
            (L"AoSCAD-UpdateInfo-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    } catch (const std::exception&) {
        result.message = L"Не удалось получить временную папку Windows.";
        CoUninitialize();
        postWorkerResult(state, kMessageCheckComplete, std::move(result));
        return;
    }
    const HRESULT downloadResult = downloadFile(kUpdateInfoUrl, manifestPath, state.window, state.cancelRequested);
    if (FAILED(downloadResult)) {
        result.cancelled = downloadResult == E_ABORT;
        result.message = result.cancelled ? L"Проверка отменена." :
            L"Не удалось загрузить описание релиза (" + hresultText(downloadResult) + L"). Проверьте подключение к интернету.";
        std::error_code ignored;
        std::filesystem::remove(manifestPath, ignored);
        CoUninitialize();
        postWorkerResult(state, kMessageCheckComplete, std::move(result));
        return;
    }

    std::string json;
    std::string versionText;
    std::string hashText;
    if (state.cancelRequested.load()) {
        result.cancelled = true;
        result.message = L"Проверка отменена.";
    } else if (!readTextFile(manifestPath, json) ||
        !jsonStringValue(json, "version", versionText) ||
        !jsonStringValue(json, "sha256", hashText)) {
        result.message = L"Файл описания обновления повреждён или имеет неподдерживаемый формат.";
    } else {
        result.version = utf8ToWide(versionText);
        result.sha256 = utf8ToWide(hashText);
        if (parseVersion(result.version).empty() || !validSha256(result.sha256)) {
            result.message = L"В релизе AoSCAD опубликованы некорректные сведения об обновлении.";
        } else {
            std::wstring installedVersion = kCurrentVersion;
            if (state.parentPid == 0) {
                installedVersion = versionFromExecutable(state.installDirectory / L"aoscad-studio.exe");
                if (installedVersion.empty())
                    installedVersion = L"0.0.0";
            }
            result.success = true;
            result.updateAvailable = versionIsNewer(result.version, installedVersion);
            result.message = result.updateAvailable ? L"Доступна новая версия AoSCAD Studio." :
                L"Установлена актуальная версия AoSCAD Studio.";
        }
    }

    std::error_code ignored;
    std::filesystem::remove(manifestPath, ignored);
    CoUninitialize();
    postWorkerResult(state, kMessageCheckComplete, std::move(result));
}

std::filesystem::path newInstallerPath()
{
    std::wstring safeVersion = kCurrentVersion;
    for (wchar_t& ch : safeVersion) {
        if (!((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-'))
            ch = L'_';
    }
    return std::filesystem::temp_directory_path() /
        (L"AoSCAD-Studio-Setup-Update-" + safeVersion + L"-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".exe");
}

void downloadUpdate(AppState& state)
{
    WorkerResult result;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        result.message = L"Не удалось инициализировать сетевой загрузчик.";
        postWorkerResult(state, kMessageDownloadComplete, std::move(result));
        return;
    }

    try {
        result.installerPath = newInstallerPath();
    } catch (const std::exception&) {
        result.message = L"Не удалось создать файл установщика во временной папке.";
        CoUninitialize();
        postWorkerResult(state, kMessageDownloadComplete, std::move(result));
        return;
    }
    postStatus(state, L"Загрузка установщика AoSCAD Studio…");
    const HRESULT downloadResult = downloadFile(kLatestInstallerUrl, result.installerPath, state.window, state.cancelRequested);
    if (FAILED(downloadResult)) {
        result.cancelled = downloadResult == E_ABORT;
        result.message = result.cancelled ? L"Загрузка отменена." :
            L"Не удалось загрузить установщик (" + hresultText(downloadResult) + L").";
        std::error_code ignored;
        std::filesystem::remove(result.installerPath, ignored);
    } else if (state.cancelRequested.load()) {
        result.cancelled = true;
        result.message = L"Загрузка отменена.";
        std::error_code ignored;
        std::filesystem::remove(result.installerPath, ignored);
    } else {
        std::wstring actualHash;
        if (!calculateSha256(result.installerPath, actualHash)) {
            result.message = L"Не удалось проверить SHA-256 установщика.";
            std::error_code ignored;
            std::filesystem::remove(result.installerPath, ignored);
        } else if (_wcsicmp(actualHash.c_str(), state.expectedSha256.c_str()) != 0) {
            result.message = L"SHA-256 не совпал с опубликованным манифестом. Установщик удалён; запуск отменён.";
            std::error_code ignored;
            std::filesystem::remove(result.installerPath, ignored);
        } else {
            result.success = true;
            result.message = L"Установщик загружен и проверен. Можно установить обновление.";
        }
    }
    CoUninitialize();
    postWorkerResult(state, kMessageDownloadComplete, std::move(result));
}

bool waitForParentProcess(AppState& state, std::wstring& error)
{
    if (state.parentPid == 0)
        return true;

    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, state.parentPid);
    if (!process) {
        if (GetLastError() == ERROR_INVALID_PARAMETER)
            return true;
        error = L"Не удалось дождаться закрытия AoSCAD Studio.";
        return false;
    }

    postStatus(state, L"Закройте AoSCAD Studio. Установка продолжится автоматически после выхода.");
    bool exited = false;
    while (!state.cancelRequested.load()) {
        const DWORD waitResult = WaitForSingleObject(process, 300);
        if (waitResult == WAIT_OBJECT_0) {
            exited = true;
            break;
        }
        if (waitResult == WAIT_FAILED) {
            error = L"Ошибка ожидания закрытия приложения.";
            break;
        }
    }
    CloseHandle(process);
    if (!exited && state.cancelRequested.load())
        error = L"Установка отменена.";
    return exited;
}

bool runInstaller(const std::filesystem::path& installerPath,
                  const std::filesystem::path& installDirectory,
                  bool silent,
                  DWORD& exitCode,
                  std::wstring& error)
{
    std::wstring commandLine = quoteArgument(installerPath.wstring());
    if (silent)
        commandLine += L" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-";
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const wchar_t* workingDirectory = installDirectory.empty() ? nullptr : installDirectory.c_str();
    if (!CreateProcessW(installerPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        silent ? CREATE_NO_WINDOW : 0, nullptr, workingDirectory, &startupInfo, &processInfo)) {
        error = L"Не удалось запустить установщик Windows (код " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    CloseHandle(processInfo.hThread);
    if (!silent) {
        CloseHandle(processInfo.hProcess);
        exitCode = 0;
        return true;
    }

    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        error = L"Ожидание завершения установки завершилось ошибкой.";
        CloseHandle(processInfo.hProcess);
        return false;
    }
    if (!GetExitCodeProcess(processInfo.hProcess, &exitCode)) {
        error = L"Не удалось получить результат установки.";
        CloseHandle(processInfo.hProcess);
        return false;
    }
    CloseHandle(processInfo.hProcess);
    if (exitCode != 0) {
        error = L"Установщик завершился с кодом " + std::to_wstring(exitCode) + L".";
        return false;
    }
    return true;
}

void installUpdate(AppState& state)
{
    WorkerResult result;
    result.installerPath = state.installerPath;
    if (state.parentPid == 0) {
        state.installationStarted = true;
        PostMessageW(state.window, kMessageInstalling, 0, 0);
        DWORD exitCode = 0;
        if (runInstaller(state.installerPath, state.installDirectory, false, exitCode, result.message)) {
            result.success = true;
            result.message = L"Установщик запущен. Завершите установку в появившемся окне.";
        }
        postWorkerResult(state, kMessageInstallComplete, std::move(result));
        return;
    }

    if (!waitForParentProcess(state, result.message)) {
        result.cancelled = state.cancelRequested.load();
        postWorkerResult(state, kMessageInstallComplete, std::move(result));
        return;
    }

    if (state.cancelRequested.load()) {
        result.cancelled = true;
        result.message = L"Установка отменена.";
        postWorkerResult(state, kMessageInstallComplete, std::move(result));
        return;
    }

    state.installationStarted = true;
    PostMessageW(state.window, kMessageInstalling, 0, 0);
    postStatus(state, L"Установка обновления…");
    DWORD exitCode = 0;
    if (runInstaller(state.installerPath, state.installDirectory, true, exitCode, result.message)) {
        const std::filesystem::path applicationPath = state.installDirectory / L"aoscad-studio.exe";
        const HINSTANCE launchResult = ShellExecuteW(nullptr, L"open", applicationPath.c_str(), nullptr,
                                                       state.installDirectory.c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launchResult) <= 32) {
            result.message = L"AoSCAD Studio обновлён, но не удалось автоматически запустить приложение.";
            result.success = false;
        } else {
            result.success = true;
            result.message = L"AoSCAD Studio обновлён и запущен.";
        }
    }
    postWorkerResult(state, kMessageInstallComplete, std::move(result));
}

void joinWorker(AppState& state)
{
    if (state.worker.joinable())
        state.worker.join();
    state.workerRunning = false;
}

void startCheck(AppState& state)
{
    state.stage = Stage::Checking;
    state.cancelRequested = false;
    state.workerRunning = true;
    state.installationStarted = false;
    EnableWindow(state.primaryButton, FALSE);
    EnableWindow(state.cancelButton, TRUE);
    SetWindowTextW(state.primaryButton, L"Проверка…");
    SendMessageW(state.progressBar, PBM_SETMARQUEE, TRUE, 30);
    state.worker = std::thread(checkForUpdates, std::ref(state));
}

void startDownload(AppState& state)
{
    state.stage = Stage::Downloading;
    state.cancelRequested = false;
    state.workerRunning = true;
    EnableWindow(state.primaryButton, FALSE);
    EnableWindow(state.cancelButton, TRUE);
    SetWindowTextW(state.primaryButton, L"Загрузка…");
    SendMessageW(state.progressBar, PBM_SETPOS, 0, 0);
    SendMessageW(state.progressBar, PBM_SETMARQUEE, FALSE, 0);
    SendMessageW(state.progressBar, PBM_SETSTATE, PBST_NORMAL, 0);
    SendMessageW(state.progressBar, PBM_SETMARQUEE, TRUE, 30);
    state.worker = std::thread(downloadUpdate, std::ref(state));
}

void startInstall(AppState& state)
{
    state.stage = Stage::Installing;
    state.cancelRequested = false;
    state.workerRunning = true;
    state.installationStarted = false;
    EnableWindow(state.primaryButton, FALSE);
    EnableWindow(state.cancelButton, TRUE);
    SetWindowTextW(state.primaryButton, L"Установка…");
    SendMessageW(state.progressBar, PBM_SETMARQUEE, TRUE, 30);
    state.worker = std::thread(installUpdate, std::ref(state));
}

void setFinishedState(AppState& state, const std::wstring& message)
{
    state.stage = Stage::Finished;
    SetWindowTextW(state.statusLabel, message.c_str());
    SendMessageW(state.progressBar, PBM_SETMARQUEE, FALSE, 0);
    EnableWindow(state.primaryButton, TRUE);
    EnableWindow(state.cancelButton, TRUE);
    SetWindowTextW(state.primaryButton, L"Закрыть");
    SetWindowTextW(state.cancelButton, L"Закрыть");
}

void layoutControls(AppState& state, int width, int height)
{
    const int margin = 20;
    const int contentWidth = std::max(100, width - margin * 2);
    MoveWindow(state.statusLabel, margin, 24, contentWidth, 52, TRUE);
    MoveWindow(state.progressBar, margin, 84, contentWidth, 18, TRUE);
    const int buttonWidth = 150;
    MoveWindow(state.primaryButton, width - margin * 2 - buttonWidth * 2 - 10, height - 48,
               buttonWidth, 28, TRUE);
    MoveWindow(state.cancelButton, width - margin - buttonWidth, height - 48, buttonWidth, 28, TRUE);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<AppState*>(creation->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE: {
        state->statusLabel = CreateWindowExW(0, L"STATIC", L"Подготовка проверки обновлений…",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 100, 30, window, nullptr, nullptr, nullptr);
        state->progressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH | PBS_MARQUEE, 0, 0, 100, 18, window, nullptr, nullptr, nullptr);
        state->primaryButton = CreateWindowExW(0, L"BUTTON", L"Проверка…",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 100, 28,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPrimaryButtonId)), nullptr, nullptr);
        state->cancelButton = CreateWindowExW(0, L"BUTTON", L"Отмена",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 100, 28,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)), nullptr, nullptr);
        const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SendMessageW(state->statusLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(state->primaryButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(state->cancelButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(state->progressBar, PBM_SETRANGE32, 0, 100);
        startCheck(*state);
        return 0;
    }
    case WM_SIZE:
        layoutControls(*state, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kPrimaryButtonId) {
            if (state->stage == Stage::ReadyToDownload)
                startDownload(*state);
            else if (state->stage == Stage::Downloaded)
                startInstall(*state);
            else if (state->stage == Stage::Finished)
                DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == kCancelButtonId) {
            if (state->workerRunning) {
                if (!state->installationStarted) {
                    state->cancelRequested = true;
                    EnableWindow(state->cancelButton, FALSE);
                    postStatus(*state, L"Отмена операции…");
                }
            } else {
                DestroyWindow(window);
            }
            return 0;
        }
        break;
    case kMessageStatus: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        if (text)
            SetWindowTextW(state->statusLabel, text->c_str());
        return 0;
    }
    case kMessageProgress:
        if (lParam == 0) {
            SendMessageW(state->progressBar, PBM_SETMARQUEE, TRUE, 30);
        } else {
            SendMessageW(state->progressBar, PBM_SETMARQUEE, FALSE, 0);
            SendMessageW(state->progressBar, PBM_SETPOS, wParam, 0);
        }
        return 0;
    case kMessageCheckComplete: {
        joinWorker(*state);
        std::unique_ptr<WorkerResult> result(reinterpret_cast<WorkerResult*>(lParam));
        if (!result)
            return 0;
        SendMessageW(state->progressBar, PBM_SETMARQUEE, FALSE, 0);
        if (result->cancelled) {
            setFinishedState(*state, L"Проверка отменена.");
        } else if (!result->success) {
            setFinishedState(*state, result->message);
        } else if (!result->updateAvailable) {
            setFinishedState(*state, L"Установлена актуальная версия AoSCAD Studio " + std::wstring(kCurrentVersion) + L".");
        } else {
            state->latestVersion = result->version;
            state->expectedSha256 = result->sha256;
            state->stage = Stage::ReadyToDownload;
            SetWindowTextW(state->statusLabel, (L"Доступна новая версия " + state->latestVersion + L". Нажмите кнопку, чтобы загрузить и проверить установщик.").c_str());
            SetWindowTextW(state->primaryButton, L"Скачать и установить");
            EnableWindow(state->primaryButton, TRUE);
            EnableWindow(state->cancelButton, TRUE);
        }
        return 0;
    }
    case kMessageDownloadComplete: {
        joinWorker(*state);
        std::unique_ptr<WorkerResult> result(reinterpret_cast<WorkerResult*>(lParam));
        if (!result)
            return 0;
        SendMessageW(state->progressBar, PBM_SETMARQUEE, FALSE, 0);
        if (!result->success) {
            setFinishedState(*state, result->message);
        } else {
            state->installerPath = result->installerPath;
            state->stage = Stage::Downloaded;
            SetWindowTextW(state->statusLabel, L"Установщик загружен и SHA-256 проверен. Нажмите «Установить».");
            SetWindowTextW(state->primaryButton, L"Установить");
            EnableWindow(state->primaryButton, TRUE);
            EnableWindow(state->cancelButton, TRUE);
            SendMessageW(state->progressBar, PBM_SETPOS, 100, 0);
        }
        return 0;
    }
    case kMessageInstalling:
        EnableWindow(state->cancelButton, FALSE);
        SetWindowTextW(state->cancelButton, L"Подождите…");
        return 0;
    case kMessageInstallComplete: {
        joinWorker(*state);
        state->installationStarted = false;
        std::unique_ptr<WorkerResult> result(reinterpret_cast<WorkerResult*>(lParam));
        if (!result)
            return 0;
        setFinishedState(*state, result->message);
        return 0;
    }
    case WM_CLOSE:
        if (state->workerRunning) {
            if (!state->installationStarted) {
                state->cancelRequested = true;
                postStatus(*state, L"Отмена операции…");
            }
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->cancelRequested = true;
        if (state->worker.joinable())
            state->worker.join();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    const std::wstring originalPath = modulePath();
    if (originalPath.empty()) {
        MessageBoxW(nullptr, L"Не удалось определить путь к updater.", L"AoSCAD Studio Update", MB_OK | MB_ICONERROR);
        return 1;
    }
    const Arguments arguments = parseArguments();
    if (!relaunchFromTemporaryDirectory(arguments, originalPath)) {
        MessageBoxW(nullptr, L"Не удалось запустить updater из временной папки Windows.",
                    L"AoSCAD Studio Update", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!arguments.workerCopy)
        return 0;

    const HRESULT comResult = OleInitialize(nullptr);
    if (FAILED(comResult)) {
        MessageBoxW(nullptr, L"Не удалось инициализировать сетевой компонент Windows.",
                    L"AoSCAD Studio Update", MB_OK | MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&commonControls);

    AppState state;
    state.parentPid = arguments.parentPid;
    state.installDirectory = arguments.installDirectory.empty()
        ? std::filesystem::path(directoryOf(originalPath))
        : arguments.installDirectory;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        OleUninitialize();
        MessageBoxW(nullptr, L"Не удалось создать окно updater.", L"AoSCAD Studio Update", MB_OK | MB_ICONERROR);
        return 1;
    }

    const int width = 480;
    const int height = 190;
    RECT bounds{0, 0, width, height};
    AdjustWindowRect(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    const int windowWidth = bounds.right - bounds.left;
    const int windowHeight = bounds.bottom - bounds.top;
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, L"AoSCAD Studio — Обновление",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        std::max(0, (screenWidth - windowWidth) / 2), std::max(0, (screenHeight - windowHeight) / 2),
        windowWidth, windowHeight, nullptr, nullptr, instance, &state);
    if (!window) {
        OleUninitialize();
        MessageBoxW(nullptr, L"Не удалось открыть окно updater.", L"AoSCAD Studio Update", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    OleUninitialize();
    return static_cast<int>(message.wParam);
}
