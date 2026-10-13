#!/usr/bin/env python3
"""Validate SignPath's test-signing policy and expose its public certificate."""
import hashlib, json, os, pathlib, urllib.request

base = "https://app.signpath.io/Api/v1/"
org = os.environ["SIGNPATH_ORGANIZATION_ID"]
project = os.environ["SIGNPATH_PROJECT_SLUG"]
token = os.environ["SIGNPATH_TOKEN"]
headers = {"Authorization": f"Bearer {token}", "Accept": "application/json"}

def get(path, accept="application/json"):
    request_headers = {**headers, "Accept": accept}
    req = urllib.request.Request(base + org + path, headers=request_headers)
    with urllib.request.urlopen(req) as response:
        return response.read()

policy = json.loads(get(f"/Projects/{project}/SigningPolicies/test-signing"))
if policy.get("status") != "Valid":
    raise SystemExit(f"test-signing policy status is {policy.get('status')!r}, expected 'Valid'")
if policy.get("purpose") != "TestSigning":
    raise SystemExit(f"test-signing policy purpose is {policy.get('purpose')!r}, expected 'TestSigning'")
cert = get(f"/Projects/{project}/SigningPolicies/test-signing/Certificate/X509Certificate", "application/pkix-cert")
out = pathlib.Path(os.environ.get("RUNNER_TEMP", "/tmp")) / "signpath-test.cer"
out.write_bytes(cert)
sha = hashlib.sha256(cert).hexdigest()
version = pathlib.Path("version.txt").read_text().strip()
with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as f:
    f.write(f"certificate_sha256={sha}\nversion={version}\n")
print(f"Validated test-signing policy; certificate SHA256: {sha}")
