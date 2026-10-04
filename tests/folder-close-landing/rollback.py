from pathlib import Path
import sys,json,zipfile,hashlib
here=Path(__file__).resolve().parent
root=Path(sys.argv[1]).resolve()
assert root.is_dir()
modified=json.loads((here/'modified-hashes.json').read_text())
for n,s in modified.items():
 p=(root/n).resolve();assert p.is_relative_to(root)
 assert p.is_file() and hashlib.sha256(p.read_bytes()).hexdigest()==s, 'target differs: '+n
with zipfile.ZipFile(here/'ORIGINAL_FILE.zip') as z:
 for n in modified:
  p=root/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(z.read(n))
original=json.loads((here/'original-hashes.json').read_text())
assert all(hashlib.sha256((root/n).read_bytes()).hexdigest()==s for n,s in original.items())
print('ROLLBACK_HASHES=PASS; restored five original files')
