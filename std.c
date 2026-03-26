int _fltused = 0;

void *memcpy(void *dest, const void *src, size_t size)
{
    unsigned char *dest_bytes = (unsigned char *)dest;
    const unsigned char *src_bytes = (const unsigned char *)src;
    size_t i;

    for (i = 0; i < size; ++i) {
        dest_bytes[i] = src_bytes[i];
    }

    return dest;
}

void *memset(void *dest, int value, size_t size)
{
    unsigned char *dest_bytes = (unsigned char *)dest;
    size_t i;

    for (i = 0; i < size; ++i) {
        dest_bytes[i] = (unsigned char)value;
    }

    return dest;
}

size_t strlen(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0') {
        ++length;
    }

    return length;
}

static void *AppMalloc(size_t size)
{
    return HeapAlloc(GetProcessHeap(), 0, size);
}

static void AppFree(void *memory)
{
    if (memory != NULL) {
        HeapFree(GetProcessHeap(), 0, memory);
    }
}

static void *AppMemcpy(void *dest, const void *src, size_t size)
{
    return memcpy(dest, src, size);
}

static void *AppMemset(void *dest, int value, size_t size)
{
    return memset(dest, value, size);
}

static size_t AppStrlen(const char *text)
{
    return strlen(text);
}

static bool AppIsValidHandle(HANDLE handle)
{
    return handle != NULL && handle != INVALID_HANDLE_VALUE;
}

static HANDLE AppGetStdoutHandle(void)
{
    static HANDLE cached_stdout = NULL;

    if (AppIsValidHandle(cached_stdout)) {
        return cached_stdout;
    }

    cached_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (AppIsValidHandle(cached_stdout)) {
        return cached_stdout;
    }

    AttachConsole(ATTACH_PARENT_PROCESS);
    cached_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (AppIsValidHandle(cached_stdout)) {
        return cached_stdout;
    }

    cached_stdout = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (AppIsValidHandle(cached_stdout)) {
        SetStdHandle(STD_OUTPUT_HANDLE, cached_stdout);
        return cached_stdout;
    }

    return INVALID_HANDLE_VALUE;
}

static void AppWriteStdout(const char *text)
{
    HANDLE stdout_handle = AppGetStdoutHandle();
    DWORD bytes_written = 0;
    size_t length;

    if (!AppIsValidHandle(stdout_handle)) {
        OutputDebugStringA(text);
        return;
    }

    length = AppStrlen(text);
    if (length == 0 || length > MAXDWORD) {
        return;
    }

    if (!WriteFile(stdout_handle, text, (DWORD)length, &bytes_written, NULL)) {
        OutputDebugStringA(text);
    }
}

static void AppWriteStdoutLine(const char *text)
{
    AppWriteStdout(text);
    AppWriteStdout("\r\n");
}

static void AppFormatHrMessage(char *buffer, const char *context, HRESULT hr)
{
    wsprintfA(buffer, "%s failed: 0x%08lX.", context, (unsigned long)hr);
}

static void AppFormatScaleLabel(char *buffer, float scale)
{
    int scaled = (int)(scale * 100.0f + 0.5f);
    int whole = scaled / 100;
    int fraction = scaled % 100;

    wsprintfA(buffer, "Text scale %d.%02dx  (-/=)", whole, fraction);
}

static bool AppCopyString(char *dest, size_t capacity, const char *src)
{
    size_t i = 0;

    if (capacity == 0) {
        return false;
    }

    while (src[i] != '\0') {
        if (i + 1 >= capacity) {
            dest[0] = '\0';
            return false;
        }
        dest[i] = src[i];
        ++i;
    }

    dest[i] = '\0';
    return true;
}

static bool AppAppendString(char *dest, size_t capacity, const char *src)
{
    size_t dest_len = AppStrlen(dest);
    size_t i = 0;

    while (src[i] != '\0') {
        if (dest_len + i + 1 >= capacity) {
            return false;
        }
        dest[dest_len + i] = src[i];
        ++i;
    }

    dest[dest_len + i] = '\0';
    return true;
}

static void AppTrimToDirectory(char *path)
{
    size_t len = AppStrlen(path);

    while (len > 0) {
        --len;
        if (path[len] == '\\' || path[len] == '/') {
            path[len] = '\0';
            return;
        }
    }

    path[0] = '\0';
}

static HANDLE AppOpenFileForRead(const char *path)
{
    return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void AppWritePathTrace(const char *prefix, const char *path)
{
    AppWriteStdout(prefix);
    AppWriteStdoutLine(path);
}

static void AppWriteWin32Error(const char *prefix, DWORD error_code)
{
    char buffer[128];

    wsprintfA(buffer, "%s0x%08lX", prefix, (unsigned long)error_code);
    AppWriteStdoutLine(buffer);
}

static void AppWriteSizeTrace(const char *prefix, unsigned long value)
{
    char buffer[128];

    wsprintfA(buffer, "%s%lu", prefix, value);
    AppWriteStdoutLine(buffer);
}

static bool AppLoadEntireFile(const char *path, unsigned char **data_out, size_t *size_out)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    char module_dir[MAX_PATH];
    char candidate[MAX_PATH];
    const char *resolved_path = NULL;
    LARGE_INTEGER file_size = {0};
    unsigned char *data = NULL;
    DWORD bytes_read = 0;

    *data_out = NULL;
    *size_out = 0;

    AppWritePathTrace("AppLoadEntireFile try: ", path);
    file = AppOpenFileForRead(path);
    if (file != INVALID_HANDLE_VALUE) {
        resolved_path = path;
    }
    if (file == INVALID_HANDLE_VALUE) {
        DWORD module_len = GetModuleFileNameA(NULL, module_dir, MAX_PATH);

        if (module_len == 0 || module_len >= MAX_PATH) {
            AppWriteStdoutLine("unable to get filename");
            return false;
        }

        AppTrimToDirectory(module_dir);

        if (AppCopyString(candidate, MAX_PATH, module_dir) &&
                AppAppendString(candidate, MAX_PATH, "\\") &&
                AppAppendString(candidate, MAX_PATH, path)) {
            AppWritePathTrace("AppLoadEntireFile try: ", candidate);
            file = AppOpenFileForRead(candidate);
            if (file != INVALID_HANDLE_VALUE) {
                resolved_path = candidate;
            }
        }

        if (file == INVALID_HANDLE_VALUE &&
                AppCopyString(candidate, MAX_PATH, module_dir) &&
                AppAppendString(candidate, MAX_PATH, "\\..\\") &&
                AppAppendString(candidate, MAX_PATH, path)) {
            AppWritePathTrace("AppLoadEntireFile try: ", candidate);
            file = AppOpenFileForRead(candidate);
            if (file != INVALID_HANDLE_VALUE) {
                resolved_path = candidate;
            }
        }

        if (file == INVALID_HANDLE_VALUE) {
            AppWriteStdoutLine("AppLoadEntireFile failed to open any candidate path.");
            return false;
        }
    }

    if (resolved_path != NULL) {
        AppWritePathTrace("AppLoadEntireFile resolved: ", resolved_path);
    }

    if (!GetFileSizeEx(file, &file_size)) {
        AppWriteWin32Error("GetFileSizeEx failed, error=", GetLastError());
        CloseHandle(file);
        return false;
    }

    if (file_size.QuadPart <= 0 || (ULONGLONG)file_size.QuadPart > MAXDWORD) {
        AppWriteSizeTrace("AppLoadEntireFile rejected size: ", (unsigned long)file_size.QuadPart);
        CloseHandle(file);
        return false;
    }

    AppWriteSizeTrace("AppLoadEntireFile size: ", (unsigned long)file_size.QuadPart);

    data = (unsigned char *)AppMalloc((size_t)file_size.QuadPart);
    if (data == NULL) {
        AppWriteStdoutLine("AppMalloc failed in AppLoadEntireFile.");
        CloseHandle(file);
        return false;
    }

    if (!ReadFile(file, data, (DWORD)file_size.QuadPart, &bytes_read, NULL) || bytes_read != (DWORD)file_size.QuadPart) {
        AppWriteWin32Error("ReadFile failed, error=", GetLastError());
        AppWriteSizeTrace("ReadFile bytes_read: ", (unsigned long)bytes_read);
        AppFree(data);
        CloseHandle(file);
        return false;
    }

    CloseHandle(file);
    *data_out = data;
    *size_out = (size_t)file_size.QuadPart;
    return true;
}
