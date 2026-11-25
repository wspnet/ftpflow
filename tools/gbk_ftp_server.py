# gbk_ftp_server.py -- a local FTP server that serves filenames in GBK bytes,
# mimicking a Chinese IIS FTP (which lists names in code page 936) so we can
# verify the GBK -> UTF-8 -> GBK round-trip for navigating Chinese directories.
import os
import shutil
import tempfile

from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer

ROOT = tempfile.mkdtemp(prefix="ftpflow_gbk_")

# Create a Chinese directory and a Chinese file inside it.
os.mkdir(os.path.join(ROOT, "中文目录"))
with open(os.path.join(ROOT, "中文目录", "中文文件.txt"), "wb") as f:
    f.write("hello from a gbk-named file\n".encode("utf-8"))
with open(os.path.join(ROOT, "根文件.txt"), "wb") as f:
    f.write("root file\n".encode("utf-8"))


def _gbk(s):
    # Encode a unicode name to GBK bytes, then decode as latin-1 so the
    # byte string keeps the exact GBK octets on the wire.
    return s.encode("gbk", errors="replace").decode("latin-1")


class GBKFTPHandler(FTPHandler):
    # The whole point of this server is to emit GBK bytes for filenames,
    # exactly like a Chinese IIS FTP. pyftpdlib's format_list/format_mlsx
    # encode the rendered line with self.cmd_channel.encoding (default
    # utf-8), so setting it to "gbk" makes the wire bytes genuinely GBK
    # instead of UTF-8 -- which is what the auto-detect + lock path must
    # handle.
    encoding = "gbk"


authorizer = DummyAuthorizer()
authorizer.add_user("gbkuser", "gbkpass", ROOT, perm="elradfmwMT")
handler = GBKFTPHandler
handler.authorizer = authorizer

server = FTPServer(("127.0.0.1", 2121), handler)
print("GBK FTP server on 127.0.0.1:2121, root =", ROOT)
server.serve_forever()
