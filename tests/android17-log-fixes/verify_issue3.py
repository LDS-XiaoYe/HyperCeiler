"""Check that missing SuperLyric service does not trigger registration noise."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else (
    ROOT / "library/libhook/src/main/java/com/sevtinge/hyperceiler/libhook/appbase/systemui/MusicBaseHook.kt"
)
text = source.read_text(encoding="utf-8")
method = text.split("private fun registerLyricReceiver()", 1)[1].split("abstract fun onSuperLyric", 1)[0]
guarded = bool(re.search(
    r"if \(!SuperLyricHelper\.isAvailable\(\)\)\s*return@runCatching"
    r".*?SuperLyricHelper\.registerReceiver\(receiver\)", method, re.S
))
unrelated_errors_preserved = "XposedLog.e(TAG, lpparam.packageName" in method
print(f"{'PASS' if guarded else 'FAIL'}: unavailable service skips registration")
print(f"{'PASS' if unrelated_errors_preserved else 'FAIL'}: unexpected registration failures remain visible")
sys.exit(0 if guarded and unrelated_errors_preserved else 1)
