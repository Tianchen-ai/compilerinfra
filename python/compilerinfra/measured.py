"""Reader for an observed final artifact issued by the native product compiler."""
import hashlib
import json


def unpack(contents):
    magic, length, body = contents.split(b"\n", 2)
    if magic != b"CIMEAS1" or not length.isdigit() or int(length) > len(body):
        raise ValueError("invalid measured artifact header")
    size = int(length)
    header, payload = json.loads(body[:size]), body[size:]
    if hashlib.sha256(payload).hexdigest() != header["artifact_sha256"]:
        raise ValueError("measured artifact payload differs from header")
    header["package_sha256"] = hashlib.sha256(contents).hexdigest()
    return header, payload
