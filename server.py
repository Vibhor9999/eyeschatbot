"""
Relay server for the ESP32 eyes chatbot (Hindi + English).

  ESP32 mic audio (raw 16 kHz, 16-bit mono PCM)
     -> faster-whisper  (speech to text, auto-detects Hindi / English)
     -> Grok (xAI) API  (answer)
     -> edge-tts        (Hindi / English voice)
     -> ffmpeg          (convert to raw 16 kHz PCM)
     -> back to ESP32 with an X-Mood header that controls the eyes

Install:
    pip install fastapi uvicorn faster-whisper openai edge-tts numpy
    + install ffmpeg  (Windows: winget install ffmpeg | Linux: apt install ffmpeg | Mac: brew install ffmpeg)

Run:
    set XAI_API_KEY=xai-...        (Windows)   /   export XAI_API_KEY=...  (Linux/Mac)
    uvicorn server:app --host 0.0.0.0 --port 8000
"""
import asyncio
import datetime
import io
import os
import re
import time
import wave

from openai import OpenAI
import edge_tts
import numpy as np
from fastapi import FastAPI, Request, Response
try:
    from faster_whisper import WhisperModel      # only needed for local (offline) speech-to-text
except ImportError:
    WhisperModel = None
try:
    from gtts import gTTS                        # backup voice if edge-tts is blocked
except ImportError:
    gTTS = None
import shutil

# ---------------- settings (override with environment variables) ----------------
API_KEY        = os.environ.get("XAI_API_KEY") or os.environ.get("LLM_API_KEY") or ""
IS_GROQ        = API_KEY.startswith("gsk_")                      # gsk_ = Groq key, xai- = xAI Grok key
BASE_URL       = os.getenv("LLM_BASE_URL", "https://api.groq.com/openai/v1" if IS_GROQ else "https://api.x.ai/v1")
GROK_MODEL     = os.getenv("LLM_MODEL", "")                      # leave empty = picked automatically from your key's model list
STT_MODEL      = os.getenv("STT_MODEL", "")                      # cloud speech-to-text (Groq whisper), picked automatically
USE_CLOUD_STT  = os.getenv("USE_CLOUD_STT", "1") == "1"          # fast + better Hindi; falls back to local Whisper
WHISPER_MODEL  = os.getenv("WHISPER_MODEL", "small")             # base / small / medium (medium = best Hindi)
WHISPER_DEVICE = os.getenv("WHISPER_DEVICE", "cpu")              # "cuda" if you have an NVIDIA GPU
WHISPER_TYPE   = os.getenv("WHISPER_TYPE", "int8")               # "float16" on GPU
LOCATION       = os.getenv("DEVICE_LOCATION", "Mumbai, India")
DEVICE_TOKEN   = os.getenv("DEVICE_TOKEN", "")                  # secret shared with the ESP32 (needed when the server is public)

VOICE_HI = "hi-IN-SwaraNeural"      # alternative: hi-IN-MadhurNeural (male)
VOICE_EN = "en-IN-NeerjaNeural"     # alternative: en-IN-PrabhatNeural (male), en-US-JennyNeural
MOODS = {"normal", "happy", "sad", "surprised", "angry", "tired", "love", "wink"}
INDIC = {"hi", "ur", "mr", "ne", "pa", "gu", "bn", "sa"}         # Whisper often mislabels Hindi as one of these
# --------------------------------------------------------------------------------

app = FastAPI()
_stt = None


def local_stt():                                         # local Whisper is only loaded if it is needed
    global _stt
    if WhisperModel is None:
        raise RuntimeError("local Whisper is not installed (pip install faster-whisper)")
    if _stt is None:
        print("Loading local Whisper model (first time only)...")
        _stt = WhisperModel(WHISPER_MODEL, device=WHISPER_DEVICE, compute_type=WHISPER_TYPE)
    return _stt


grok = OpenAI(api_key=API_KEY, base_url=BASE_URL, timeout=40)

# --- startup: check the key and pick working models automatically ---
def _pick(ids, prefs, bad):
    ids = [i for i in ids if not any(w in i.lower() for w in bad)]
    for p in prefs:
        for i in ids:
            if p in i.lower():
                return i
    return ids[0] if ids else ""


print(f"\nService: {BASE_URL}   key: {API_KEY[:6]}...{API_KEY[-4:] if len(API_KEY) > 10 else ''}")
try:
    _ids = sorted(m.id for m in grok.models.list().data)
    print("Models your key can use:", ", ".join(_ids))
    if not GROK_MODEL:
        GROK_MODEL = _pick(_ids, ["llama-3.3-70b", "llama-3.1-8b", "llama", "gpt-oss", "qwen", "gemma", "kimi", "grok"],
                           ["whisper", "tts", "guard", "orpheus", "playai", "embed", "compound", "safeguard"])
    if not STT_MODEL and IS_GROQ:
        STT_MODEL = _pick([i for i in _ids if "whisper" in i.lower()], ["large-v3-turbo", "large-v3", "whisper"], [])
    if GROK_MODEL not in _ids:
        print(f"\n!!! WARNING: model '{GROK_MODEL}' is not in your list.\n")
    print(f"Chat model : {GROK_MODEL}")
    print(f"Speech model: {STT_MODEL or '(local Whisper)'}\n")
except Exception as _e:
    print(f"\n!!! The key does not work or the service is unreachable: {_e}\n")
if not IS_GROQ:
    STT_MODEL = ""                                       # only Groq is used for cloud speech-to-text here
history: list[dict] = []
last_used = 0.0


def system_prompt() -> str:
    now = datetime.datetime.now().strftime("%A, %d %B %Y, %I:%M %p")
    return (
        "You are the voice of a small, friendly robot with expressive cartoon eyes. "
        "Your words are spoken aloud through a speaker, so write exactly how a person would talk.\n"
        "- Reply in the language the person used: if they spoke Hindi or Hinglish, answer in Hindi written in "
        "Devanagari script; if they spoke English, answer in English.\n"
        "- Keep it to one to three short sentences. No markdown, lists, emojis, URLs or symbols.\n"
        "- Start every reply with exactly one mood tag in square brackets that fits the answer: "
        "[normal] [happy] [sad] [surprised] [angry] [tired] [love] [wink].\n"
        f"- Current date and time: {now}. The robot is in {LOCATION}."
    )


HALLUCINATIONS = {"", ".", "mm", "mm.", "hmm", "hmm.", "you", "you.", "thank you.", "thank you", "thanks for watching!",
                  "thanks for watching.", "bye.", "bye", "okay.", "uh", "um"}


def wav_bytes(audio: np.ndarray) -> bytes:
    pcm16 = (np.clip(audio, -1, 1) * 32767).astype(np.int16)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm16.tobytes())
    return buf.getvalue()


def transcribe_cloud(audio: np.ndarray) -> str:
    kw = dict(model=STT_MODEL, file=("speech.wav", wav_bytes(audio)), temperature=0.0)
    text = grok.audio.transcriptions.create(**kw).text.strip()
    if re.search(r"[\u0600-\u06FF]", text):               # Hindi written in Urdu script -> redo as Hindi
        text = grok.audio.transcriptions.create(language="hi", **kw).text.strip()
    return text


def transcribe_local(audio: np.ndarray) -> str:
    stt = local_stt()
    segs, info = stt.transcribe(audio, beam_size=1, vad_filter=True)
    text = " ".join(s.text.strip() for s in segs).strip()
    if info.language not in ("en", "hi"):                # re-run in a language we actually support
        lang = "hi" if info.language in INDIC else "en"
        segs, _ = stt.transcribe(audio, language=lang, beam_size=1, vad_filter=True)
        text = " ".join(s.text.strip() for s in segs).strip()
    return text


def transcribe(pcm: bytes) -> str:
    pcm = pcm[: len(pcm) // 2 * 2]
    audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    if len(audio) < 16000 * 0.5:
        return ""
    peak = float(np.abs(audio).max())
    if peak < 0.005:
        return ""
    audio *= min(0.9 / peak, 30.0)                       # normalise a quiet mic

    text = ""
    if USE_CLOUD_STT and STT_MODEL:
        try:
            text = transcribe_cloud(audio)
        except Exception as e:
            print("Cloud speech-to-text failed, using local Whisper:", e)
            text = transcribe_local(audio)
    else:
        text = transcribe_local(audio)
    return "" if text.strip().lower() in HALLUCINATIONS else text.strip()


def ask_grok(user_text: str) -> tuple[str, str]:
    global last_used
    if time.time() - last_used > 300:                    # forget the chat after 5 minutes of silence
        history.clear()
    last_used = time.time()

    history.append({"role": "user", "content": user_text})
    try:
        kwargs = dict(
            model=GROK_MODEL,
            max_tokens=1500,                             # thinking models spend part of this on hidden reasoning
            messages=[{"role": "system", "content": system_prompt()}] + history[-8:],
        )
        if "gpt-oss" in GROK_MODEL:
            kwargs["reasoning_effort"] = "low"           # much faster answers for voice
        r = grok.chat.completions.create(**kwargs)
        text = (r.choices[0].message.content or "").strip()
        if not text:
            raise RuntimeError("empty answer from the model")
    except Exception as e:                               # network / key / quota problems
        print("LLM error:", e)
        history.pop()
        return "Sorry, I could not think of an answer right now.", "sad"

    mood = "happy"
    m = re.match(r"\s*\[(\w+)\]\s*", text)
    if m:
        if m.group(1).lower() in MOODS:
            mood = m.group(1).lower()
        text = text[m.end():]
    text = re.sub(r"\[\w+\]", "", text).strip()
    history.append({"role": "assistant", "content": text})
    return text, mood


def clean_for_speech(text: str) -> str:
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"[^\w\s.,!?;:'\-%/()\u0900-\u097F]", " ", text)   # drop markdown, emojis, symbols
    return re.sub(r"\s+", " ", text).strip()


def ffmpeg_path() -> str:
    p = shutil.which("ffmpeg")
    if p:
        return p
    import imageio_ffmpeg                         # bundled ffmpeg (used on cloud hosts)
    return imageio_ffmpeg.get_ffmpeg_exe()


def gtts_mp3(text: str, lang: str) -> bytes:
    buf = io.BytesIO()
    gTTS(text=text, lang=lang, tld="co.in").write_to_fp(buf)
    return buf.getvalue()


async def synth(text: str) -> bytes:
    hindi = bool(re.search(r"[\u0900-\u097F]", text))
    mp3 = b""
    try:
        async for chunk in edge_tts.Communicate(text, VOICE_HI if hindi else VOICE_EN).stream():
            if chunk["type"] == "audio":
                mp3 += chunk["data"]
    except Exception as e:
        print("edge-tts failed:", e)
    if not mp3 and gTTS is not None:
        print("using backup voice (gTTS)")
        mp3 = await asyncio.to_thread(gtts_mp3, text, "hi" if hindi else "en")
    if not mp3:
        return b""
    p = await asyncio.create_subprocess_exec(
        ffmpeg_path(), "-loglevel", "error", "-i", "pipe:0",
        "-f", "s16le", "-ar", "16000", "-ac", "1", "pipe:1",
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
    )
    pcm, _ = await p.communicate(mp3)
    return pcm


@app.get("/health")
async def health():
    return {"ok": True}


@app.post("/chat")
async def chat(request: Request):
    if DEVICE_TOKEN and request.headers.get("x-device-token") != DEVICE_TOKEN:
        return Response(status_code=401)
    try:
        pcm = await request.body()
    except Exception:                                    # ESP32 closed the connection early
        return Response(status_code=204)
    t0 = time.time()
    text = await asyncio.to_thread(transcribe, pcm)
    print(f"\nYou   : {text!r}   ({time.time() - t0:.1f}s)")
    if not text:
        return Response(status_code=204)

    reply, mood = await asyncio.to_thread(ask_grok, text)
    print(f"Robot : [{mood}] {reply}   ({time.time() - t0:.1f}s)")
    audio = await synth(clean_for_speech(reply))
    print(f"Done in {time.time() - t0:.1f}s, {len(audio)} bytes")
    if not audio:
        return Response(status_code=500)
    return Response(content=audio, media_type="application/octet-stream", headers={"X-Mood": mood})
