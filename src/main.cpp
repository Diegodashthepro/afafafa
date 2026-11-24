// Imagen a Bloques (Geode 6 / GD 2.2091)
//
// Importa una imagen (.png/.jpg) o un video corto (.mp4, max 10 s) al editor.
//  - Imagen: bloques (objeto 211) escalados; zonas de colores parecidos se unen
//    en un solo bloque. Se respeta un maximo de objetos bajando la resolucion.
//  - Video: los bloques se crean una vez; cada zona usa un canal de color y
//    se agregan triggers de color por cuadro (sin agregar mas bloques).
//
// Todo lo creado lleva el grupo 9990.

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/async.hpp>
#include <atomic>
#include <thread>

#ifdef GEODE_IS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#endif

#ifdef GEODE_IS_ANDROID
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

using namespace geode::prelude;

static constexpr int kBlockID   = 211;   // cuadrado de color plano (30x30)
static constexpr int kTriggerID = 899;   // trigger de color
static constexpr int kGroup     = 9990;
static const float kSpeeds[5]   = {251.16f, 311.58f, 387.42f, 468.0f, 576.0f};
static const char* kSpeedNames[5] = {"0.5x", "1x", "2x", "3x", "4x"};

static void notify(std::string const& text, bool ok = true) {
    Notification::create(text, ok ? NotificationIcon::Success : NotificationIcon::Error)->show();
}

// ===========================================================================
// Opciones
// ===========================================================================
struct Options {
    int preset = 0;          // 0 = 480x960, 1 = 720x1080
    int maxObjects = 8000;
    int maxColors = 128;     // solo imagen
    int tolerance = 14;      // 0-100 (diferencia maxima por canal RGB)
    int widthBlocks = 20;    // ancho final en bloques de GD
    int fps = 6;             // solo video
    int speed = 1;           // indice en kSpeeds
    int firstChannel = 1;
    bool swapUV = false;
    bool smooth = true;      // video: transiciones con fade (menos triggers)
    int mode = 0;            // 0 = bloques, 1 = trazos horizontales, 2 = trazos verticales
    bool removeBg = false;   // imagen: no crear bloques del color de fondo

    void load() {
        auto m = Mod::get();
        preset = m->getSavedValue<int>("preset", 0);
        maxObjects = m->getSavedValue<int>("max-objects", 8000);
        maxColors = m->getSavedValue<int>("max-colors", 128);
        tolerance = m->getSavedValue<int>("tolerance", 14);
        widthBlocks = m->getSavedValue<int>("width-blocks", 20);
        fps = m->getSavedValue<int>("fps", 6);
        speed = m->getSavedValue<int>("speed", 1);
        firstChannel = m->getSavedValue<int>("first-channel", 1);
        swapUV = m->getSavedValue<bool>("swap-uv", false);
        smooth = m->getSavedValue<bool>("smooth", true);
        mode = m->getSavedValue<int>("mode", 0);
        removeBg = m->getSavedValue<bool>("remove-bg", false);
    }
    void save() const {
        auto m = Mod::get();
        m->setSavedValue("preset", preset);
        m->setSavedValue("max-objects", maxObjects);
        m->setSavedValue("max-colors", maxColors);
        m->setSavedValue("tolerance", tolerance);
        m->setSavedValue("width-blocks", widthBlocks);
        m->setSavedValue("fps", fps);
        m->setSavedValue("speed", speed);
        m->setSavedValue("first-channel", firstChannel);
        m->setSavedValue("swap-uv", swapUV);
        m->setSavedValue("smooth", smooth);
        m->setSavedValue("mode", mode);
        m->setSavedValue("remove-bg", removeBg);
    }
    int channelsAvailable() const { return std::max(1, 999 - firstChannel + 1); }
    void presetSize(bool landscape, int& w, int& h) const {
        int a = preset == 0 ? 480 : 720, b = preset == 0 ? 960 : 1080;
        w = landscape ? b : a;
        h = landscape ? a : b;
    }
};
static Options g_opt;

// ===========================================================================
// Imagenes en memoria
// ===========================================================================
struct RGB { float r = 0, g = 0, b = 0; };

struct Img {
    int w = 0, h = 0;
    std::vector<RGB> px;
    std::vector<uint8_t> alpha; // 255 = opaco
    RGB& at(int x, int y) { return px[y * w + x]; }
};

// Reduce la imagen para que entre en maxW x maxH (promedio por area)
static Img fitInside(Img const& src, int maxW, int maxH) {
    float s = std::min(1.f, std::min(static_cast<float>(maxW) / src.w, static_cast<float>(maxH) / src.h));
    int w = std::max(1, static_cast<int>(src.w * s)), h = std::max(1, static_cast<int>(src.h * s));
    if (w == src.w && h == src.h) return src;
    Img out;
    out.w = w; out.h = h;
    out.px.resize(w * h);
    out.alpha.resize(w * h);
    for (int y = 0; y < h; y++) {
        int y0 = y * src.h / h, y1 = std::max(y0 + 1, (y + 1) * src.h / h);
        for (int x = 0; x < w; x++) {
            int x0 = x * src.w / w, x1 = std::max(x0 + 1, (x + 1) * src.w / w);
            RGB acc; float a = 0; int n = 0;
            for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
                auto& p = src.px[yy * src.w + xx];
                acc.r += p.r; acc.g += p.g; acc.b += p.b;
                a += src.alpha[yy * src.w + xx];
                n++;
            }
            out.px[y * w + x] = {acc.r / n, acc.g / n, acc.b / n};
            out.alpha[y * w + x] = static_cast<uint8_t>(a / n);
        }
    }
    return out;
}

static bool decodeImage(std::string const& path, Img& out, std::string& err) {
    auto data = file::readBinary(path);
    if (!data.isOk()) { err = "No se pudo leer la imagen"; return false; }
    auto bytes = std::move(data).unwrap();
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    bool jpg = lower.ends_with(".jpg") || lower.ends_with(".jpeg");
    auto img = new CCImage();
    bool ok = img->initWithImageData(bytes.data(), static_cast<int>(bytes.size()),
                                     jpg ? CCImage::kFmtJpg : CCImage::kFmtPng);
    if (!ok) {
        // probar el otro formato por si la extension miente
        ok = img->initWithImageData(bytes.data(), static_cast<int>(bytes.size()),
                                    jpg ? CCImage::kFmtPng : CCImage::kFmtJpg);
    }
    if (!ok || !img->getData()) { img->release(); err = "Formato de imagen no soportado (usa PNG o JPG)"; return false; }
    int w = img->getWidth(), h = img->getHeight();
    bool alpha = img->hasAlpha();
    int bpp = alpha ? 4 : 3;
    auto d = img->getData();
    out.w = w; out.h = h;
    out.px.resize(w * h);
    out.alpha.resize(w * h);
    for (int i = 0; i < w * h; i++) {
        out.px[i] = {static_cast<float>(d[i * bpp]), static_cast<float>(d[i * bpp + 1]), static_cast<float>(d[i * bpp + 2])};
        out.alpha[i] = alpha ? d[i * bpp + 3] : 255;
    }
    img->release();
    return true;
}

// ===========================================================================
// Video (Android NDK MediaCodec)
// ===========================================================================
static bool decodeVideo(std::string const& path, Options const& o, std::vector<Img>& frames, std::string& err) {
#ifdef GEODE_IS_ANDROID
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { err = "No se pudo abrir el video"; return false; }
    struct stat st{};
    fstat(fd, &st);
    AMediaExtractor* ex = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) != AMEDIA_OK) {
        AMediaExtractor_delete(ex); close(fd);
        err = "Formato de video no soportado";
        return false;
    }
    int track = -1;
    AMediaFormat* fmt = nullptr;
    const char* mime = nullptr;
    for (size_t i = 0; i < AMediaExtractor_getTrackCount(ex); i++) {
        auto f = AMediaExtractor_getTrackFormat(ex, i);
        const char* m = nullptr;
        if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m && std::strncmp(m, "video/", 6) == 0) {
            track = static_cast<int>(i); fmt = f; mime = m;
            break;
        }
        AMediaFormat_delete(f);
    }
    if (track < 0) { AMediaExtractor_delete(ex); close(fd); err = "El archivo no tiene video"; return false; }
    AMediaExtractor_selectTrack(ex, track);
    std::string mimeStr = mime;
    AMediaCodec* codec = AMediaCodec_createDecoderByType(mimeStr.c_str());
    if (!codec || AMediaCodec_configure(codec, fmt, nullptr, nullptr, 0) != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK) {
        if (codec) AMediaCodec_delete(codec);
        AMediaFormat_delete(fmt); AMediaExtractor_delete(ex); close(fd);
        err = "Este celular no puede decodificar ese video";
        return false;
    }

    int32_t width = 0, height = 0;
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &width);
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &height);
    int32_t stride = width, slice = height, colorFormat = 21;
    int32_t cropL = 0, cropT = 0, cropR = width - 1, cropB = height - 1;
    auto readOut = [&] {
        AMediaFormat* of = AMediaCodec_getOutputFormat(codec);
        if (!of) return;
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_WIDTH, &width);
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_HEIGHT, &height);
        stride = width; slice = height;
        AMediaFormat_getInt32(of, "stride", &stride);
        AMediaFormat_getInt32(of, "slice-height", &slice);
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_COLOR_FORMAT, &colorFormat);
        cropL = 0; cropT = 0; cropR = width - 1; cropB = height - 1;
        AMediaFormat_getInt32(of, "crop-left", &cropL);
        AMediaFormat_getInt32(of, "crop-top", &cropT);
        AMediaFormat_getInt32(of, "crop-right", &cropR);
        AMediaFormat_getInt32(of, "crop-bottom", &cropB);
        if (stride < width) stride = width;
        if (slice < height) slice = height;
        AMediaFormat_delete(of);
    };

    const int64_t step = 1000000LL / std::max(1, o.fps);
    const int64_t maxT = 10LL * 1000000LL;
    int64_t nextT = 0;
    bool inEOS = false;
    int idle = 0;

    auto convert = [&](uint8_t const* buf, size_t size) {
        int cw = cropR - cropL + 1, ch = cropB - cropT + 1;
        if (cw <= 0 || ch <= 0) return;
        int maxW, maxH;
        o.presetSize(cw > ch, maxW, maxH);
        // para procesar alcanza con ~160 px de ancho (las zonas son <= 999)
        float s = std::min({1.f, static_cast<float>(maxW) / cw, static_cast<float>(maxH) / ch, 160.f / std::max(cw, ch) * 1.8f});
        int tw = std::max(4, static_cast<int>(cw * s)), th = std::max(4, static_cast<int>(ch * s));
        Img f;
        f.w = tw; f.h = th;
        f.px.resize(tw * th);
        f.alpha.assign(tw * th, 255);
        bool planar = colorFormat == 19;
        size_t ySize = static_cast<size_t>(stride) * slice;
        for (int y = 0; y < th; y++) {
            int sy = cropT + std::min(ch - 1, static_cast<int>((y + 0.5f) * ch / th));
            for (int x = 0; x < tw; x++) {
                int sx = cropL + std::min(cw - 1, static_cast<int>((x + 0.5f) * cw / tw));
                size_t yi = static_cast<size_t>(sy) * stride + sx;
                size_t ui, vi;
                if (planar) {
                    size_t cstride = stride / 2;
                    ui = ySize + static_cast<size_t>(sy / 2) * cstride + sx / 2;
                    vi = ySize + cstride * (slice / 2) + static_cast<size_t>(sy / 2) * cstride + sx / 2;
                } else {
                    ui = ySize + static_cast<size_t>(sy / 2) * stride + (sx / 2) * 2;
                    vi = ui + 1;
                }
                if (yi >= size || ui >= size || vi >= size) continue;
                float Y = buf[yi], U = buf[ui], V = buf[vi];
                if (o.swapUV) std::swap(U, V);
                float c = 1.164f * (Y - 16.f);
                float r = c + 1.596f * (V - 128.f);
                float g = c - 0.392f * (U - 128.f) - 0.813f * (V - 128.f);
                float b = c + 2.017f * (U - 128.f);
                f.px[y * tw + x] = {std::clamp(r, 0.f, 255.f), std::clamp(g, 0.f, 255.f), std::clamp(b, 0.f, 255.f)};
            }
        }
        frames.push_back(std::move(f));
    };

    while (true) {
        if (!inEOS) {
            ssize_t ii = AMediaCodec_dequeueInputBuffer(codec, 5000);
            if (ii >= 0) {
                size_t cap = 0;
                uint8_t* buf = AMediaCodec_getInputBuffer(codec, ii, &cap);
                ssize_t sz = AMediaExtractor_readSampleData(ex, buf, cap);
                int64_t t = AMediaExtractor_getSampleTime(ex);
                if (sz < 0 || t > maxT + 300000) {
                    AMediaCodec_queueInputBuffer(codec, ii, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    inEOS = true;
                } else {
                    AMediaCodec_queueInputBuffer(codec, ii, 0, sz, t, 0);
                    AMediaExtractor_advance(ex);
                }
            }
        }
        AMediaCodecBufferInfo info{};
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(codec, &info, 5000);
        if (oi >= 0) {
            idle = 0;
            if (info.size > 0 && info.presentationTimeUs >= nextT && info.presentationTimeUs <= maxT) {
                size_t osz = 0;
                uint8_t* ob = AMediaCodec_getOutputBuffer(codec, oi, &osz);
                if (ob) convert(ob + info.offset, static_cast<size_t>(info.size));
                while (nextT <= info.presentationTimeUs) nextT += step;
            }
            bool end = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) || info.presentationTimeUs > maxT;
            AMediaCodec_releaseOutputBuffer(codec, oi, false);
            if (end) break;
        } else if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            readOut();
        } else if (oi == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            if (inEOS && ++idle > 400) break;
        }
        if (frames.size() >= static_cast<size_t>(10 * std::max(1, o.fps) + 1)) break;
    }

    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    AMediaFormat_delete(fmt);
    AMediaExtractor_delete(ex);
    close(fd);
    if (frames.empty()) { err = "No se pudieron leer cuadros del video"; return false; }
    // todos los cuadros al mismo tamano
    int w = frames[0].w, h = frames[0].h;
    for (auto& f : frames) if (f.w != w || f.h != h) f = fitInside(f, w, h);
    frames.erase(std::remove_if(frames.begin(), frames.end(), [&](Img const& f) { return f.w != w || f.h != h; }), frames.end());
    return true;
#elif defined(GEODE_IS_WINDOWS)
    // Windows: Media Foundation (incluido en Windows), salida RGB32
    bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(MFStartup(MF_VERSION))) {
        if (comInit) CoUninitialize();
        err = "No se pudo iniciar Media Foundation";
        return false;
    }
    auto finish = [&](bool ok) {
        MFShutdown();
        if (comInit) CoUninitialize();
        return ok;
    };

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(std::max(0, wlen)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 1);
    if (attrs) attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs, &reader);
    if (attrs) attrs->Release();
    if (FAILED(hr) || !reader) { err = "Formato de video no soportado"; return finish(false); }

    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    IMFMediaType* outType = nullptr;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(stream, nullptr, outType);
    outType->Release();
    if (FAILED(hr)) { reader->Release(); err = "No se pudo convertir el video a RGB"; return finish(false); }

    IMFMediaType* cur = nullptr;
    UINT32 width = 0, height = 0;
    LONG stride = 0;
    if (SUCCEEDED(reader->GetCurrentMediaType(stream, &cur)) && cur) {
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &width, &height);
        UINT32 st = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) stride = static_cast<LONG>(st);
        cur->Release();
    }
    if (width == 0 || height == 0) { reader->Release(); err = "No se pudo leer el tamano del video"; return finish(false); }
    if (stride == 0) stride = static_cast<LONG>(width * 4);

    const LONGLONG step = 10000000LL / std::max(1, o.fps);   // unidades de 100 ns
    const LONGLONG maxT = 10LL * 10000000LL;
    LONGLONG nextT = 0;
    while (frames.size() < static_cast<size_t>(10 * std::max(1, o.fps) + 1)) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(stream, 0, &idx, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { if (sample) sample->Release(); break; }
        if (!sample) continue;
        if (ts > maxT) { sample->Release(); break; }
        if (ts >= nextT) {
            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&data, nullptr, &len)) && data) {
                    int cw = static_cast<int>(width), ch = static_cast<int>(height);
                    int maxW, maxH;
                    o.presetSize(cw > ch, maxW, maxH);
                    float s = std::min({1.f, static_cast<float>(maxW) / cw, static_cast<float>(maxH) / ch,
                                        160.f / std::max(cw, ch) * 1.8f});
                    int tw = std::max(4, static_cast<int>(cw * s)), th = std::max(4, static_cast<int>(ch * s));
                    Img f;
                    f.w = tw; f.h = th;
                    f.px.resize(tw * th);
                    f.alpha.assign(tw * th, 255);
                    LONG absStride = stride < 0 ? -stride : stride;
                    for (int y = 0; y < th; y++) {
                        int sy = std::min(ch - 1, static_cast<int>((y + 0.5f) * ch / th));
                        // stride negativo = filas de abajo hacia arriba
                        size_t row = stride < 0 ? static_cast<size_t>(ch - 1 - sy) * absStride : static_cast<size_t>(sy) * absStride;
                        for (int x = 0; x < tw; x++) {
                            int sx = std::min(cw - 1, static_cast<int>((x + 0.5f) * cw / tw));
                            size_t i = row + static_cast<size_t>(sx) * 4;
                            if (i + 2 >= len) continue;
                            // RGB32 = B, G, R, X
                            f.px[y * tw + x] = {static_cast<float>(data[i + 2]), static_cast<float>(data[i + 1]),
                                                static_cast<float>(data[i])};
                        }
                    }
                    frames.push_back(std::move(f));
                    buf->Unlock();
                }
                buf->Release();
            }
            while (nextT <= ts) nextT += step;
        }
        sample->Release();
    }
    reader->Release();
    if (frames.empty()) { err = "No se pudieron leer cuadros del video"; return finish(false); }
    return finish(true);
#else
    err = "El video no esta soportado en esta plataforma";
    return false;
#endif
}

// ===========================================================================
// Procesamiento
// ===========================================================================
struct Rect { int x, y, w, h; int color; };

struct BuildResult {
    std::string error;
    int gw = 0, gh = 0;
    std::vector<Rect> rects;               // color = indice de canal relativo
    std::vector<RGB> palette;              // imagen: color de cada canal
    std::vector<std::vector<RGB>> frames;  // video: [cuadro][zona]
    bool video = false;
};

static float diff(RGB const& a, RGB const& b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

// Promedio de la imagen en una grilla gw x gh
static void gridColors(Img const& img, int gw, int gh, std::vector<RGB>& col, std::vector<bool>& solid) {
    col.assign(gw * gh, {});
    solid.assign(gw * gh, true);
    for (int cy = 0; cy < gh; cy++) {
        int y0 = cy * img.h / gh, y1 = std::max(y0 + 1, (cy + 1) * img.h / gh);
        for (int cx = 0; cx < gw; cx++) {
            int x0 = cx * img.w / gw, x1 = std::max(x0 + 1, (cx + 1) * img.w / gw);
            RGB acc; float a = 0; int n = 0;
            for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
                auto& p = img.px[y * img.w + x];
                acc.r += p.r; acc.g += p.g; acc.b += p.b;
                a += img.alpha[y * img.w + x];
                n++;
            }
            col[cy * gw + cx] = {acc.r / n, acc.g / n, acc.b / n};
            solid[cy * gw + cx] = a / n >= 128.f;
        }
    }
}

// Une celdas vecinas parecidas.
//  shape 0: rectangulo creciendo primero a lo ancho
//  shape 1: rectangulo creciendo primero a lo alto
//  shape 2: trazo horizontal (1 celda de alto)
//  shape 3: trazo vertical (1 celda de ancho)
template <class Close>
static std::vector<Rect> greedyShape(int gw, int gh, std::vector<bool> const& solid, Close&& close, int shape) {
    std::vector<bool> used(gw * gh, false);
    std::vector<Rect> rects;
    auto free = [&](int x, int y, int seed) {
        int j = y * gw + x;
        return !used[j] && solid[j] && close(seed, j);
    };
    auto visit = [&](int x, int y) {
        int i = y * gw + x;
        if (used[i] || !solid[i]) return;
        int w = 1, h = 1;
        if (shape == 0 || shape == 2) {
            while (x + w < gw && free(x + w, y, i)) w++;
            if (shape == 0) {
                while (y + h < gh) {
                    bool ok = true;
                    for (int k = 0; k < w && ok; k++) ok = free(x + k, y + h, i);
                    if (!ok) break;
                    h++;
                }
            }
        } else {
            while (y + h < gh && free(x, y + h, i)) h++;
            if (shape == 1) {
                while (x + w < gw) {
                    bool ok = true;
                    for (int k = 0; k < h && ok; k++) ok = free(x + w, y + k, i);
                    if (!ok) break;
                    w++;
                }
            }
        }
        for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++) used[yy * gw + xx] = true;
        rects.push_back({x, y, w, h, 0});
    };
    if (shape == 1 || shape == 3) {
        for (int x = 0; x < gw; x++) for (int y = 0; y < gh; y++) visit(x, y);
    } else {
        for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) visit(x, y);
    }
    return rects;
}

// Segun el modo: bloques (prueba las 2 direcciones y usa la que da menos
// objetos) o trazos de 1 objeto por linea
template <class Close>
static std::vector<Rect> greedyRects(int gw, int gh, std::vector<bool> const& solid, Close&& close, int mode = 0) {
    if (mode == 1) return greedyShape(gw, gh, solid, close, 2);
    if (mode == 2) return greedyShape(gw, gh, solid, close, 3);
    auto a = greedyShape(gw, gh, solid, close, 0);
    auto b = greedyShape(gw, gh, solid, close, 1);
    return b.size() < a.size() ? b : a;
}

// Color de fondo: el mas comun en el borde de la imagen
static bool backgroundColor(std::vector<RGB> const& col, std::vector<bool> const& solid, int gw, int gh, float tol, RGB& out) {
    std::vector<RGB> border;
    for (int x = 0; x < gw; x++) { border.push_back(col[x]); border.push_back(col[(gh - 1) * gw + x]); }
    for (int y = 0; y < gh; y++) { border.push_back(col[y * gw]); border.push_back(col[y * gw + gw - 1]); }
    int best = -1, bestN = 0;
    for (size_t i = 0; i < border.size(); i += std::max<size_t>(1, border.size() / 64)) {
        int n = 0;
        for (auto& c : border) if (diff(c, border[i]) <= tol) n++;
        if (n > bestN) { bestN = n; best = static_cast<int>(i); }
    }
    // solo si de verdad domina el borde
    if (best < 0 || bestN < static_cast<int>(border.size() * 0.4f)) return false;
    out = border[best];
    (void)solid;
    return true;
}

// Paleta por "median cut" (colores con peso)
static std::vector<RGB> medianCut(std::vector<std::pair<RGB, float>> items, int k) {
    struct Box { std::vector<std::pair<RGB, float>> v; };
    std::vector<Box> boxes{{std::move(items)}};
    auto range = [](Box const& b, int& ch) {
        float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
        for (auto& [c, w] : b.v) {
            float v[3] = {c.r, c.g, c.b};
            for (int i = 0; i < 3; i++) { mn[i] = std::min(mn[i], v[i]); mx[i] = std::max(mx[i], v[i]); }
        }
        ch = 0;
        float best = -1;
        for (int i = 0; i < 3; i++) if (mx[i] - mn[i] > best) { best = mx[i] - mn[i]; ch = i; }
        return best;
    };
    while (static_cast<int>(boxes.size()) < k) {
        int bi = -1, bch = 0;
        float best = 0.5f;
        for (size_t i = 0; i < boxes.size(); i++) {
            if (boxes[i].v.size() < 2) continue;
            int ch;
            float r = range(boxes[i], ch);
            if (r > best) { best = r; bi = static_cast<int>(i); bch = ch; }
        }
        if (bi < 0) break;
        auto& v = boxes[bi].v;
        std::sort(v.begin(), v.end(), [bch](auto const& a, auto const& b) {
            float va = bch == 0 ? a.first.r : bch == 1 ? a.first.g : a.first.b;
            float vb = bch == 0 ? b.first.r : bch == 1 ? b.first.g : b.first.b;
            return va < vb;
        });
        Box second;
        second.v.assign(v.begin() + v.size() / 2, v.end());
        v.resize(v.size() / 2);
        boxes.push_back(std::move(second));
    }
    std::vector<RGB> pal;
    for (auto& b : boxes) {
        RGB acc; float tw = 0;
        for (auto& [c, w] : b.v) { acc.r += c.r * w; acc.g += c.g * w; acc.b += c.b * w; tw += w; }
        if (tw > 0) pal.push_back({acc.r / tw, acc.g / tw, acc.b / tw});
    }
    return pal;
}

static BuildResult processImage(Img const& src, Options const& o) {
    BuildResult res;
    int pw, ph;
    o.presetSize(src.w > src.h, pw, ph);
    Img img = fitInside(src, pw, ph);
    float tol = static_cast<float>(o.tolerance);

    std::vector<RGB> col;
    std::vector<bool> solid;
    std::vector<Rect> rects;
    int gw = 4, gh = 1;

    auto evaluate = [&](int w) {
        gw = w;
        gh = std::max(1, static_cast<int>(std::round(gw * static_cast<float>(img.h) / img.w)));
        gridColors(img, gw, gh, col, solid);
        if (o.removeBg) {
            RGB bg;
            if (backgroundColor(col, solid, gw, gh, tol, bg)) {
                for (int i = 0; i < gw * gh; i++) if (diff(col[i], bg) <= tol) solid[i] = false;
            }
        }
        rects = greedyRects(gw, gh, solid, [&](int a, int b) { return diff(col[a], col[b]) <= tol; }, o.mode);
        return static_cast<int>(rects.size());
    };

    // busqueda binaria: la grilla MAS GRANDE que entra en el maximo de objetos
    int lo = 4, hi = std::min(img.w, 600), bestW = 4;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (evaluate(mid) <= o.maxObjects) { bestW = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    evaluate(bestW);
    if (rects.empty()) { res.error = "No quedo nada (la imagen es solo fondo o transparente)"; return res; }

    // color promedio de cada rectangulo y paleta para los canales
    std::vector<RGB> rectCol(rects.size());
    std::vector<std::pair<RGB, float>> items;
    for (size_t i = 0; i < rects.size(); i++) {
        auto& r = rects[i];
        RGB acc; int n = 0;
        for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) {
            auto& c = col[y * gw + x];
            acc.r += c.r; acc.g += c.g; acc.b += c.b; n++;
        }
        rectCol[i] = {acc.r / n, acc.g / n, acc.b / n};
        items.push_back({rectCol[i], static_cast<float>(n)});
    }
    int k = std::max(1, std::min(o.maxColors, o.channelsAvailable()));
    res.palette = medianCut(std::move(items), k);
    if (res.palette.empty()) { res.error = "La imagen esta vacia o es transparente"; return res; }
    for (size_t i = 0; i < rects.size(); i++) {
        int best = 0;
        float bd = 1e9f;
        for (size_t p = 0; p < res.palette.size(); p++) {
            auto& a = rectCol[i]; auto& b = res.palette[p];
            float d = (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b);
            if (d < bd) { bd = d; best = static_cast<int>(p); }
        }
        rects[i].color = best;
    }
    res.gw = gw; res.gh = gh;
    res.rects = std::move(rects);
    return res;
}

// Quita el "parpadeo" del ruido de camara: mediana de 3 cuadros por celda
static void temporalDenoise(std::vector<std::vector<RGB>>& cols) {
    int F = static_cast<int>(cols.size());
    if (F < 3) return;
    size_t N = cols[0].size();
    auto med3 = [](float a, float b, float c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); };
    std::vector<RGB> prev(N), cur(N);
    prev = cols[0];
    for (int f = 1; f < F - 1; f++) {
        cur = cols[f];
        for (size_t i = 0; i < N; i++) {
            auto& a = prev[i]; auto& b = cur[i]; auto& c = cols[f + 1][i];
            cols[f][i] = {med3(a.r, b.r, c.r), med3(a.g, b.g, c.g), med3(a.b, b.b, c.b)};
        }
        prev = cur;
    }
}

static BuildResult processVideo(std::vector<Img> const& frames, Options const& o) {
    BuildResult res;
    res.video = true;
    auto const& f0 = frames[0];
    float tol = static_cast<float>(o.tolerance);
    int maxChannels = o.channelsAvailable();
    int F = static_cast<int>(frames.size());

    auto seriesClose = [&](std::vector<std::vector<RGB>> const& cols, int a, int b) {
        for (int f = 0; f < F; f++) if (diff(cols[f][a], cols[f][b]) > tol) return false;
        return true;
    };

    int gw = 4, gh = 1;
    std::vector<std::vector<RGB>> cols(F);       // [cuadro][celda]
    std::vector<bool> solid;
    std::vector<Rect> rects;
    std::vector<std::vector<RGB>> clusterSeries;  // [canal][cuadro]

    // devuelve true si la grilla de ancho w entra en objetos y canales
    auto evaluate = [&](int w) {
        gw = w;
        gh = std::max(1, static_cast<int>(std::round(gw * static_cast<float>(f0.h) / f0.w)));
        for (int f = 0; f < F; f++) gridColors(frames[f], gw, gh, cols[f], solid);
        solid.assign(gw * gh, true);
        // 1) celdas vecinas que se parecen en TODOS los cuadros -> un objeto
        rects = greedyRects(gw, gh, solid, [&](int a, int b) { return seriesClose(cols, a, b); }, o.mode);
        if (static_cast<int>(rects.size()) > o.maxObjects) return false;
        // 2) objetos (aunque esten lejos) que cambian igual -> comparten canal
        clusterSeries.clear();
        for (auto& r : rects) {
            int seed = r.y * gw + r.x;
            int found = -1;
            for (size_t c = 0; c < clusterSeries.size() && found < 0; c++) {
                bool ok = true;
                for (int f = 0; f < F && ok; f++) ok = diff(cols[f][seed], clusterSeries[c][f]) <= tol;
                if (ok) found = static_cast<int>(c);
            }
            if (found < 0) {
                if (static_cast<int>(clusterSeries.size()) >= maxChannels) return false;
                std::vector<RGB> ser(F);
                for (int f = 0; f < F; f++) ser[f] = cols[f][seed];
                clusterSeries.push_back(std::move(ser));
                found = static_cast<int>(clusterSeries.size()) - 1;
            }
            r.color = found;
        }
        return true;
    };

    int lo = 4, hi = std::min(f0.w, 200), bestW = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (evaluate(mid)) { bestW = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (bestW > 0) evaluate(bestW);
    else clusterSeries.clear();
    if (clusterSeries.empty() || static_cast<int>(clusterSeries.size()) > maxChannels) {
        res.error = "Demasiados colores distintos. Sube la tolerancia o baja el primer canal";
        return res;
    }

    // color real de cada canal por cuadro = promedio (por area) de sus bloques
    size_t C = clusterSeries.size();
    res.frames.assign(F, std::vector<RGB>(C));
    std::vector<float> area(C, 0.f);
    for (auto& r : rects) {
        float n = static_cast<float>(r.w * r.h);
        area[r.color] += n;
        for (int f = 0; f < F; f++) {
            RGB acc; int cnt = 0;
            for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) {
                auto& c = cols[f][y * gw + x];
                acc.r += c.r; acc.g += c.g; acc.b += c.b; cnt++;
            }
            auto& dst = res.frames[f][r.color];
            dst.r += acc.r / cnt * n; dst.g += acc.g / cnt * n; dst.b += acc.b / cnt * n;
        }
    }
    for (int f = 0; f < F; f++)
        for (size_t c = 0; c < C; c++)
            if (area[c] > 0) { auto& d = res.frames[f][c]; d.r /= area[c]; d.g /= area[c]; d.b /= area[c]; }

    // 3) segunda pasada: canales cuyo color promedio quedo casi igual -> uno solo
    {
        float mtol = tol * 0.75f;
        std::vector<int> remap(C, -1);
        std::vector<size_t> keep;
        for (size_t c = 0; c < C; c++) {
            for (size_t k : keep) {
                bool ok = true;
                for (int f = 0; f < F && ok; f++) ok = diff(res.frames[f][c], res.frames[f][k]) <= mtol;
                if (ok) { remap[c] = static_cast<int>(remap[k]); break; }
            }
            if (remap[c] < 0) { remap[c] = static_cast<int>(keep.size()); keep.push_back(c); }
        }
        if (keep.size() < C) {
            std::vector<std::vector<RGB>> merged(F, std::vector<RGB>(keep.size()));
            std::vector<float> marea(keep.size(), 0.f);
            for (size_t c = 0; c < C; c++) {
                int m = remap[c];
                marea[m] += area[c];
                for (int f = 0; f < F; f++) {
                    auto& d = merged[f][m]; auto& s = res.frames[f][c];
                    d.r += s.r * area[c]; d.g += s.g * area[c]; d.b += s.b * area[c];
                }
            }
            for (int f = 0; f < F; f++)
                for (size_t m = 0; m < keep.size(); m++)
                    if (marea[m] > 0) { auto& d = merged[f][m]; d.r /= marea[m]; d.g /= marea[m]; d.b /= marea[m]; }
            for (auto& r : rects) r.color = remap[r.color];
            res.frames = std::move(merged);
        }
    }

    res.palette = res.frames[0];
    res.gw = gw; res.gh = gh;
    res.rects = std::move(rects);
    return res;
}

// Un trigger de color: en el cuadro 'frame' empieza un fade hacia 'color'
struct ColorKey { int frame; int channel; RGB color; float duration; };

// Convierte la serie de colores de cada canal en la menor cantidad de triggers.
// Con "suave": si el color cambia de forma casi lineal durante varios cuadros,
// se usa UN trigger con fade en vez de uno por cuadro.
static std::vector<ColorKey> buildKeys(std::vector<std::vector<RGB>> const& frames, int fps, float tol, bool smooth) {
    std::vector<ColorKey> keys;
    int F = static_cast<int>(frames.size());
    if (F < 2) return keys;
    size_t C = frames[0].size();
    float dt = 1.f / std::max(1, fps);
    for (size_t c = 0; c < C; c++) {
        auto S = [&](int f) -> RGB const& { return frames[f][c]; };
        if (!smooth) {
            RGB shown = S(0);
            for (int f = 1; f < F; f++) {
                if (diff(S(f), shown) < tol) continue;
                shown = S(f);
                keys.push_back({f, static_cast<int>(c), shown, 0.f});
            }
            continue;
        }
        int k = 0;
        RGB shown = S(0);
        while (k < F - 1) {
            // extender el tramo [k, e] mientras sea casi lineal
            auto fits = [&](int e) {
                for (int m = k + 1; m < e; m++) {
                    float t = static_cast<float>(m - k) / (e - k);
                    RGB l{shown.r + (S(e).r - shown.r) * t, shown.g + (S(e).g - shown.g) * t, shown.b + (S(e).b - shown.b) * t};
                    if (diff(S(m), l) > tol) return false;
                }
                return true;
            };
            int e = k + 1;
            while (e + 1 < F && fits(e + 1)) e++;
            if (diff(S(e), shown) >= tol) {
                keys.push_back({k, static_cast<int>(c), S(e), (e - k) * dt});
                shown = S(e);
            }
            k = e;
        }
    }
    return keys;
}

// ===========================================================================
// Crear objetos en el editor (hilo principal)
// ===========================================================================
static ccColor3B toC3(RGB const& c) {
    return {static_cast<GLubyte>(std::clamp(c.r, 0.f, 255.f)), static_cast<GLubyte>(std::clamp(c.g, 0.f, 255.f)),
            static_cast<GLubyte>(std::clamp(c.b, 0.f, 255.f))};
}

static void setChannel(GJEffectManager* em, int ch, ccColor3B c) {
    if (!em) return;
    auto act = em->getColorAction(ch);
    if (!act) return;
    act->m_color = c;
    act->m_fromColor = c;
    act->m_toColor = c;
    act->m_blending = false;
    act->m_playerColor = 0;
    act->m_fromOpacity = 1.f;
    act->m_toOpacity = 1.f;
    act->m_currentOpacity = 1.f;
    act->m_copyID = 0;
}

static void applyResult(BuildResult const& res, Options const& o) {
    auto lel = LevelEditorLayer::get();
    if (!lel) { notify("Vuelve al editor para importar", false); return; }
    if (!res.error.empty()) { notify(res.error, false); return; }

    // centro de la vista actual del editor
    auto win = CCDirector::sharedDirector()->getWinSize();
    CCPoint center = lel->m_objectLayer->convertToNodeSpace(ccp(win.width / 2, win.height / 2));
    float unit = o.widthBlocks * 30.f / res.gw;  // tamano de una celda en unidades de GD
    float ox = center.x - res.gw * unit / 2;
    float oy = center.y - res.gh * unit / 2;

    std::string out;
    out.reserve(res.rects.size() * 80);
    for (auto& r : res.rects) {
        float x = ox + (r.x + r.w / 2.f) * unit;
        float y = oy + (res.gh - (r.y + r.h / 2.f)) * unit;
        float sx = (r.w * unit + 0.25f) / 30.f;  // un poquito mas grande: sin rendijas
        float sy = (r.h * unit + 0.25f) / 30.f;
        int ch = o.firstChannel + r.color;
        out += fmt::format("1,{},2,{:.1f},3,{:.1f},21,{},128,{:.3f},129,{:.3f},57,{};", kBlockID, x, y, ch, sx, sy, kGroup);
    }

    int triggers = 0;
    if (res.video && res.frames.size() > 1) {
        float spd = kSpeeds[std::clamp(o.speed, 0, 4)];
        float tol = std::max(1.f, static_cast<float>(o.tolerance) * 0.5f);
        auto keys = buildKeys(res.frames, o.fps, tol, o.smooth);
        for (auto& k : keys) {
            float tx = ox + (static_cast<float>(k.frame) / o.fps) * spd;
            auto c3 = toC3(k.color);
            out += fmt::format("1,{},2,{:.2f},3,{:.2f},23,{},7,{},8,{},9,{},10,{:.3f},35,1,57,{};",
                               kTriggerID, tx, oy - 45.f - (k.channel % 8) * 4.f, o.firstChannel + k.channel,
                               c3.r, c3.g, c3.b, k.duration, kGroup);
            triggers++;
        }
    }

    // colores iniciales de los canales
    for (size_t i = 0; i < res.palette.size(); i++) {
        auto c = toC3(res.palette[i]);
        int ch = o.firstChannel + static_cast<int>(i);
        if (lel->m_levelSettings) setChannel(lel->m_levelSettings->m_effectManager, ch, c);
        setChannel(lel->m_effectManager, ch, c);
    }

    if (!out.empty()) out.pop_back();
    lel->createObjectsFromString(out, true, true);
    lel->levelSettingsUpdated();

    if (res.video) {
        notify(fmt::format("Video: {} bloques, {} canales, {} triggers ({} cuadros)",
                           res.rects.size(), res.palette.size(), triggers, res.frames.size()));
    } else {
        notify(fmt::format("Imagen: {} bloques, {} colores (canales {}-{})", res.rects.size(), res.palette.size(),
                           o.firstChannel, o.firstChannel + static_cast<int>(res.palette.size()) - 1));
    }
}

// ===========================================================================
// Importacion en segundo plano
// ===========================================================================
static std::atomic<bool> g_busy{false};

static void startImport(std::string path, bool video, Options o) {
    if (g_busy.exchange(true)) { notify("Ya hay una importacion en curso", false); return; }
    notify(video ? "Procesando video..." : "Procesando imagen...");
    std::thread([path = std::move(path), video, o] {
        auto res = std::make_shared<BuildResult>();
        std::string err;
        if (video) {
            std::vector<Img> frames;
            if (!decodeVideo(path, o, frames, err)) res->error = err;
            else *res = processVideo(frames, o);
        } else {
            Img img;
            if (!decodeImage(path, img, err)) res->error = err;
            else *res = processImage(img, o);
        }
        geode::queueInMainThread([res, o] {
            applyResult(*res, o);
            g_busy = false;
        });
    }).detach();
}

// ===========================================================================
// Popup de opciones
// ===========================================================================
class ImportPopup : public Popup {
    struct Stepper { CCLabelBMFont* label; std::function<std::string()> text; };
    std::vector<Stepper> m_steppers;
    CCMenu* m_menu = nullptr;

public:
    static ImportPopup* create() {
        auto p = new ImportPopup();
        if (p && p->init()) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }

    CCMenuItemSpriteExtra* addBtn(const char* text, CCPoint pos, float scale, SEL_MenuHandler sel, int tag = 0) {
        auto spr = ButtonSprite::create(text);
        spr->setScale(scale);
        auto item = CCMenuItemSpriteExtra::create(spr, this, sel);
        item->setPosition(pos);
        item->setTag(tag);
        m_menu->addChild(item);
        return item;
    }

    void addRow(const char* name, float y, int tag, std::function<std::string()> text) {
        const float W = 380.f;
        auto l = CCLabelBMFont::create(name, "bigFont.fnt");
        l->setScale(0.3f);
        l->setAnchorPoint({0.f, 0.5f});
        l->setPosition({18.f, y});
        m_mainLayer->addChild(l);
        addBtn("-", {W - 150.f, y}, 0.4f, menu_selector(ImportPopup::onMinus), tag);
        auto v = CCLabelBMFont::create("", "bigFont.fnt");
        v->setScale(0.3f);
        v->setPosition({W - 95.f, y});
        m_mainLayer->addChild(v);
        addBtn("+", {W - 40.f, y}, 0.4f, menu_selector(ImportPopup::onPlus), tag);
        m_steppers.push_back({v, std::move(text)});
    }

    bool init() {
        const float W = 380.f, H = 306.f;
        if (!Popup::init(W, H)) return false;
        this->setTitle("Imagen / Video a bloques");
        g_opt.load();

        m_menu = CCMenu::create();
        m_menu->setPosition({0, 0});
        m_menu->setContentSize({W, H});
        m_mainLayer->addChild(m_menu, 5);

        float y = H - 44.f;
        const float st = 18.6f;
        addRow("Modo", y, 11, [] {
            const char* m[] = {"Bloques", "Trazos H", "Trazos V"};
            return std::string(m[std::clamp(g_opt.mode, 0, 2)]); }); y -= st;
        addRow("Resolucion base", y, 1, [] { return std::string(g_opt.preset == 0 ? "480x960" : "720x1080"); }); y -= st;
        addRow("Max. objetos", y, 2, [] { return std::to_string(g_opt.maxObjects); }); y -= st;
        addRow("Colores (imagen)", y, 3, [] { return std::to_string(g_opt.maxColors); }); y -= st;
        addRow("Tolerancia de color", y, 4, [] { return std::to_string(g_opt.tolerance); }); y -= st;
        addRow("Ancho (bloques)", y, 5, [] { return std::to_string(g_opt.widthBlocks); }); y -= st;
        addRow("FPS (video)", y, 6, [] { return std::to_string(g_opt.fps); }); y -= st;
        addRow("Velocidad (video)", y, 7, [] { return std::string(kSpeedNames[std::clamp(g_opt.speed, 0, 4)]); }); y -= st;
        addRow("Primer canal", y, 8, [] { return std::to_string(g_opt.firstChannel); }); y -= st;
        addRow("Invertir U/V (Android)", y, 9, [] { return std::string(g_opt.swapUV ? "Si" : "No"); }); y -= st;
        addRow("Transiciones suaves (video)", y, 10, [] { return std::string(g_opt.smooth ? "Si" : "No"); }); y -= st;
        addRow("Quitar fondo (imagen)", y, 12, [] { return std::string(g_opt.removeBg ? "Si" : "No"); }); y -= st;

        addBtn("Importar imagen", {W / 2 - 85.f, 24.f}, 0.55f, menu_selector(ImportPopup::onImage));
        addBtn("Importar video", {W / 2 + 85.f, 24.f}, 0.55f, menu_selector(ImportPopup::onVideo));

        refresh();
        return true;
    }

    void refresh() {
        for (auto& s : m_steppers) s.label->setString(s.text().c_str());
        g_opt.save();
    }

    void change(int tag, int dir) {
        auto& o = g_opt;
        switch (tag) {
            case 1: o.preset = 1 - o.preset; break;
            case 2: o.maxObjects = std::clamp(o.maxObjects + dir * 1000, 1000, 40000); break;
            case 3: o.maxColors = std::clamp(o.maxColors + dir * (o.maxColors >= 64 ? 32 : 8), 8, 990); break;
            case 4: o.tolerance = std::clamp(o.tolerance + dir * 2, 0, 100); break;
            case 5: o.widthBlocks = std::clamp(o.widthBlocks + dir * 2, 4, 300); break;
            case 6: o.fps = std::clamp(o.fps + dir, 1, 15); break;
            case 7: o.speed = std::clamp(o.speed + dir, 0, 4); break;
            case 8:
                if (dir > 0) o.firstChannel = o.firstChannel == 1 ? 10 : std::min(990, o.firstChannel + 10);
                else o.firstChannel = o.firstChannel <= 10 ? 1 : o.firstChannel - 10;
                break;
            case 9: o.swapUV = !o.swapUV; break;
            case 10: o.smooth = !o.smooth; break;
            case 11: o.mode = (o.mode + (dir > 0 ? 1 : 2)) % 3; break;
            case 12: o.removeBg = !o.removeBg; break;
        }
        refresh();
    }

    void onMinus(CCObject* s) { change(static_cast<CCNode*>(s)->getTag(), -1); }
    void onPlus(CCObject* s)  { change(static_cast<CCNode*>(s)->getTag(), +1); }

    void pick(bool video) {
        file::FilePickOptions opts;
        if (video) opts.filters.push_back({"Video", {"*.mp4", "*.3gp", "*.webm", "*.mkv"}});
        else opts.filters.push_back({"Imagen", {"*.png", "*.jpg", "*.jpeg"}});
        Options o = g_opt;
        async::spawn(file::pick(file::PickMode::OpenFile, std::move(opts)), [video, o](file::PickResult res) {
            if (!res.isOk()) { notify("No se pudo abrir el selector", false); return; }
            auto picked = std::move(res).unwrap();
            if (!picked) return;
            startImport(picked->string(), video, o);
        });
        this->onClose(nullptr);
    }

    void onImage(CCObject*) { pick(false); }
    void onVideo(CCObject*) { pick(true); }
};

// ===========================================================================
// Boton en el editor
// ===========================================================================
class $modify(PixelEditorUI, EditorUI) {
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto spr = ButtonSprite::create("Imagen/Video");
        spr->setScale(0.42f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PixelEditorUI::onImport));
        auto menu = CCMenu::create();
        menu->setID("menu"_spr);
        menu->addChild(btn);
        // debajo de los botones de Editor Physics (si esta instalado)
        menu->setPosition({win.width / 2, win.height - 60.f});
        this->addChild(menu, 100);
        return true;
    }

    void onImport(CCObject*) {
        if (auto p = ImportPopup::create()) p->show();
    }
};
