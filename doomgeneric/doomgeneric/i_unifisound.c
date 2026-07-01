// SFX sound backend for UniFi UDR.
//
// Replaces doomgeneric's SDL-coupled i_sound.c. Parses Doom's DMX sound lumps,
// mixes the active channels in a background thread, and pipes S16_LE stereo PCM
// to the device's own `aplay` (ALSA) -- the playback path we already know works,
// so nothing extra needs static-linking.
//
// Music is stubbed for now (a later phase can add OPL synthesis).
//
// Env tunables:
//   DOOM_AUDIODEV   ALSA device for aplay -D   (default plughw:0,0)
//   DOOM_SND        set to "0" to disable audio entirely

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>

#include "config.h"
#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "m_config.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"

// ---- config variables owned by the sound subsystem (bound below) ----------
int   snd_samplerate     = 44100;
int   snd_cachesize      = 64 * 1024 * 1024;
int   snd_maxslicetime_ms = 28;
char *snd_musiccmd       = "";
int   snd_musicdevice    = SNDDEVICE_SB;
int   snd_sfxdevice      = SNDDEVICE_SB;
static int snd_sbport = 0, snd_sbirq = 0, snd_sbdma = 0, snd_mport = 0;

// OPL music (i_oplmusic + opl_unifi); music_opl_module is declared in i_sound.h
extern char *snd_dmxoption;           // defined in i_oplmusic.c ("-opl3" enables OPL3)
extern void OPL_Unifi_Render(int16_t *out, unsigned int nframes);
extern int  opl_unifi_active;
static int  music_gain = 256;         // master music scale (DOOM_MUSICVOL, 0..512)

// ---------------------------------------------------------------------------
#define NUM_CHANNELS 16

typedef struct { int16_t *data; int len; } cached_sfx_t;

typedef struct {
    int16_t *data;
    int      len;
    int      pos;
    int      left;     // 0..255
    int      right;    // 0..255
    volatile int active;
} mixchan_t;

static mixchan_t       chans[NUM_CHANNELS];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       thr;
static volatile int    running = 0;
static int             pipe_fd = -1;
static pid_t           child_pid = -1;
static int             out_rate = 44100;
static boolean         use_sfx_prefix = true;

static const char *env_def(const char *k, const char *d) {
    const char *v = getenv(k);
    return (v && *v) ? v : d;
}

// ---- SFX caching ----------------------------------------------------------
static boolean CacheSFX(sfxinfo_t *sfx) {
    if (sfx->driver_data != NULL) return true;     // already cached
    int lumpnum = sfx->lumpnum;
    if (lumpnum < 0) return false;

    uint8_t *lump = (uint8_t *)W_CacheLumpNum(lumpnum, PU_STATIC);
    unsigned lumplen = W_LumpLength(lumpnum);

    if (lumplen < 8 || lump[0] != 0x03 || lump[1] != 0x00) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }
    int      srate  = lump[2] | (lump[3] << 8);
    unsigned length = lump[4] | (lump[5] << 8) | (lump[6] << 16) | ((unsigned)lump[7] << 24);
    if (srate <= 0 || length > lumplen - 8 || length <= 48) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    // DMX skips the first 16 and last 16 bytes; samples follow the 8-byte header.
    uint8_t *samples = lump + 24;           // +16 pad, +8 header
    unsigned nsamp   = length - 32;

    uint64_t outlen = (uint64_t)nsamp * out_rate / (unsigned)srate;
    if (outlen < 1) outlen = 1;
    int16_t *exp = malloc((size_t)outlen * sizeof(int16_t));
    if (!exp) { W_ReleaseLumpNum(lumpnum); return false; }

    for (uint64_t i = 0; i < outlen; i++) {
        uint64_t src = i * (unsigned)srate / (unsigned)out_rate;
        if (src >= nsamp) src = nsamp - 1;
        exp[i] = (int16_t)(((int)samples[src] - 128) << 8);
    }
    W_ReleaseLumpNum(lumpnum);

    cached_sfx_t *c = malloc(sizeof(cached_sfx_t));
    if (!c) { free(exp); return false; }
    c->data = exp;
    c->len  = (int)outlen;
    sfx->driver_data = c;
    return true;
}

// ---- mixing thread --------------------------------------------------------
static ssize_t full_write(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return (ssize_t)off;
}

static void *audio_thread(void *arg) {
    (void)arg;
    enum { FR = 1024 };
    static int16_t buf[FR * 2];
    static int16_t musicbuf[FR * 2];

    while (running) {
        OPL_Unifi_Render(musicbuf, FR);          // music (uses its own lock)

        pthread_mutex_lock(&mtx);
        for (int i = 0; i < FR; i++) {
            int l = 0, r = 0;
            for (int c = 0; c < NUM_CHANNELS; c++) {
                if (!chans[c].active) continue;
                int s = chans[c].data[chans[c].pos];
                l += (s * chans[c].left)  >> 8;
                r += (s * chans[c].right) >> 8;
                if (++chans[c].pos >= chans[c].len) chans[c].active = 0;
            }
            l += (musicbuf[2 * i]     * music_gain) >> 8;
            r += (musicbuf[2 * i + 1] * music_gain) >> 8;
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            buf[2 * i]     = (int16_t)l;
            buf[2 * i + 1] = (int16_t)r;
        }
        pthread_mutex_unlock(&mtx);

        if (full_write(pipe_fd, buf, sizeof(buf)) < 0) {
            running = 0;                 // aplay died -> stop feeding
            break;
        }
    }
    return NULL;
}

// ---- public SFX interface -------------------------------------------------
void I_InitSound(boolean sfx_prefix) {
    use_sfx_prefix = sfx_prefix;
    if (atoi(env_def("DOOM_SND", "1")) == 0) {
        printf("[doom-udr] audio disabled (DOOM_SND=0)\n");
        return;
    }

    out_rate = (snd_samplerate > 0) ? snd_samplerate : 44100;
    const char *dev = env_def("DOOM_AUDIODEV", "plughw:0,0");
    char ratebuf[16];
    M_snprintf(ratebuf, sizeof(ratebuf), "%d", out_rate);

    int pfd[2];
    if (pipe(pfd) < 0) { perror("[doom-udr] pipe"); return; }
    signal(SIGPIPE, SIG_IGN);

    child_pid = fork();
    if (child_pid == 0) {
        dup2(pfd[0], 0);
        close(pfd[0]); close(pfd[1]);
        char *argv[] = { "aplay", "-q", "-t", "raw", "-f", "S16_LE",
                         "-c", "2", "-r", ratebuf, "-D", (char *)dev, "-", NULL };
        execvp("aplay", argv);
        _exit(127);
    }
    if (child_pid < 0) { perror("[doom-udr] fork"); close(pfd[0]); close(pfd[1]); return; }

    close(pfd[0]);
    pipe_fd = pfd[1];
    running = 1;
    if (pthread_create(&thr, NULL, audio_thread, NULL) != 0) {
        running = 0;
        close(pipe_fd); pipe_fd = -1;
        return;
    }
    printf("[doom-udr] audio: aplay pid=%d, %d Hz stereo -> %s\n", child_pid, out_rate, dev);
}

void I_ShutdownSound(void) {
    if (!running && pipe_fd < 0) return;
    running = 0;
    pthread_join(thr, NULL);
    if (pipe_fd >= 0) { close(pipe_fd); pipe_fd = -1; }
    if (child_pid > 0) { kill(child_pid, SIGTERM); waitpid(child_pid, NULL, 0); child_pid = -1; }
}

int I_GetSfxLumpNum(sfxinfo_t *sfx) {
    char namebuf[9];
    sfxinfo_t *s = (sfx->link != NULL) ? sfx->link : sfx;
    if (use_sfx_prefix)
        M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(s->name));
    else
        M_StringCopy(namebuf, DEH_String(s->name), sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void calc_gain(int vol, int sep, int *left, int *right) {
    int l = ((254 - sep) * vol) / 127;
    int r = ((sep)       * vol) / 127;
    if (l < 0) l = 0; else if (l > 255) l = 255;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    *left = l; *right = r;
}

int I_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep) {
    if (!running || channel < 0 || channel >= NUM_CHANNELS) return -1;
    if (!CacheSFX(sfx)) return -1;
    cached_sfx_t *c = sfx->driver_data;
    int left, right;
    calc_gain(vol, sep, &left, &right);

    pthread_mutex_lock(&mtx);
    chans[channel].data   = c->data;
    chans[channel].len    = c->len;
    chans[channel].pos    = 0;
    chans[channel].left   = left;
    chans[channel].right  = right;
    chans[channel].active = 1;
    pthread_mutex_unlock(&mtx);
    return channel;
}

void I_StopSound(int channel) {
    if (channel < 0 || channel >= NUM_CHANNELS) return;
    pthread_mutex_lock(&mtx);
    chans[channel].active = 0;
    pthread_mutex_unlock(&mtx);
}

boolean I_SoundIsPlaying(int channel) {
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    return chans[channel].active != 0;
}

void I_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= NUM_CHANNELS) return;
    int left, right;
    calc_gain(vol, sep, &left, &right);
    pthread_mutex_lock(&mtx);
    chans[channel].left  = left;
    chans[channel].right = right;
    pthread_mutex_unlock(&mtx);
}

void I_UpdateSound(void)   { /* mixing happens in audio_thread */ }
void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) { (void)sounds; (void)num_sounds; }

// ---- music: OPL synthesis via i_oplmusic ----------------------------------
static const music_module_t *mus_mod = &music_opl_module;
static boolean music_ok = false;

void I_InitMusic(void) {
    if (atoi(env_def("DOOM_MUSIC", "1")) == 0) { printf("[doom-udr] music disabled\n"); return; }
    music_gain = atoi(env_def("DOOM_MUSICVOL", "256"));
    if (music_gain < 0) music_gain = 0;
    if (music_gain > 512) music_gain = 512;
    snd_dmxoption = (char *)env_def("DOOM_DMXOPTION", "-opl3");   // OPL3 mode
    if (mus_mod->Init()) {
        music_ok = true;
        printf("[doom-udr] music: OPL synth ready (gain=%d)\n", music_gain);
    } else {
        printf("[doom-udr] music: OPL init failed (no music)\n");
    }
}
void  I_ShutdownMusic(void)           { if (music_ok) mus_mod->Shutdown(); music_ok = false; }
void  I_SetMusicVolume(int v)         { if (music_ok) mus_mod->SetMusicVolume(v); }
void  I_PauseSong(void)               { if (music_ok) mus_mod->PauseMusic(); }
void  I_ResumeSong(void)              { if (music_ok) mus_mod->ResumeMusic(); }
void *I_RegisterSong(void *d, int l)  { return music_ok ? mus_mod->RegisterSong(d, l) : NULL; }
void  I_UnRegisterSong(void *h)       { if (music_ok) mus_mod->UnRegisterSong(h); }
void  I_PlaySong(void *h, boolean lp) { if (music_ok) mus_mod->PlaySong(h, lp); }
void  I_StopSong(void)                { if (music_ok) mus_mod->StopSong(); }
boolean I_MusicIsPlaying(void)        { return music_ok ? mus_mod->MusicIsPlaying() : false; }

// ---- config binding -------------------------------------------------------
void I_BindSoundVariables(void) {
    M_BindVariable("snd_musicdevice",     &snd_musicdevice);
    M_BindVariable("snd_sfxdevice",       &snd_sfxdevice);
    M_BindVariable("snd_sbport",          &snd_sbport);
    M_BindVariable("snd_sbirq",           &snd_sbirq);
    M_BindVariable("snd_sbdma",           &snd_sbdma);
    M_BindVariable("snd_mport",           &snd_mport);
    M_BindVariable("snd_maxslicetime_ms", &snd_maxslicetime_ms);
    M_BindVariable("snd_musiccmd",        &snd_musiccmd);
    M_BindVariable("snd_samplerate",      &snd_samplerate);
    M_BindVariable("snd_cachesize",       &snd_cachesize);
}
