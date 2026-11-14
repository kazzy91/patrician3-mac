"""Record of how recomp/recomp.py was forked from nfs-most-wanted-mac 52247d2 (already applied; kept for re-syncing upstream).
Fork NFS recomp.py for Patrician III: drop the speed.exe fingerprint gate, read the image base from the PE header,
and disable every NFS-only address patch (gated behind self.nfs, default False)."""
import re, sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("import exe_fingerprint  # noqa: E402\n", "")
s = re.sub(r"SPEED_EXE_SHA256 = .*\n", "", s)
s = re.sub(r"SPEED_EXE_FINGERPRINT = .*\n", "SPEED_EXE_FINGERPRINT = None\n", s)
s = s.replace('EXE = os.environ.get("NFS_SPEED_EXE") or', 'EXE = os.environ.get("P3_EXE") or')
s = s.replace("        self.fingerprint = exe_fingerprint.check(self.data, path)\n", "        self.fingerprint = None\n")
s = s.replace("        self.base = 0x400000\n", "        self.base = struct.unpack_from('<I', self.data, off + 24 + 28)[0]\n")
nfs_sites = ["self.calls and insn.address in (0x004F6D65", "self.calls and size == 4 and insn.address in POSITIONAL_MODE_READS",
             "self.calls and insn.address in POSITIONAL_DEFAULT_STORES", "self.calls and addr == 0x006069C0",
             "self.calls and i.address in car85_names", "self.calls and i.address in car85_sites",
             "self.calls and i.address == 0x00505B4C", "self.calls and i.address == 0x0044CD0A",
             "self.calls and i.address == 0x004CC3A9"]
for k in nfs_sites:
    assert s.count(k) == 1, k
    s = s.replace(k, "self.nfs and " + k)
s = s.replace("        self.calls = False ", "        self.nfs = False                      # NFS speed.exe address patches: off for other games\n        self.calls = False ", 1)
assert "self.nfs = False" in s
open(p, "w").write(s)
print("patched", len(nfs_sites), "NFS sites")
