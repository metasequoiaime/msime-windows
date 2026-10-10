import { serializeHostMessage } from '../../../../shared/messages';
import { updateConfig } from './config-sync';
import { setupToggleButton } from './shared';
import type { UpdateManifest, ValidatedUpdate } from './update-manifest';
import { compareVersions, describeInstallerTrust, mirrorDownloadUrl, parseVersion, validateManifest } from './update-manifest';

const UPDATE_MANIFEST_URL = 'https://msime.app/update.json';
const RELEASES_PAGE_URL = 'https://github.com/metasequoiaime/MSIME-Windows/releases';
// msime.app's download mirror (Aliyun OSS, Hong Kong). GitHub release downloads from mainland China often run at tens of KB/s.
const DOWNLOAD_MIRROR_PREFIX = 'https://dl.msime.app/gh/';
const WEBSITE_URL = 'https://msime.app/';
const LICENSE_URL ='https://github.com/metasequoiaime/MSIME-Windows/blob/main/LICENSE';
const PRIVACY_URL = 'https://github.com/metasequoiaime/MSIME-Windows/blob/main/PRIVACY.md';
const REQUEST_TIMEOUT_MS = 10000;

function postExternalUrl(url: string): void {
  if (window.chrome?.webview) {
    window.chrome.webview.postMessage(serializeHostMessage({ type: 'openExternalUrl', data: url }));
    return;
  }

  window.open(url, '_blank', 'noopener,noreferrer');
}

// The release pipeline publishes the installer's digest and whether it was signed. Showing both at
// the moment the user is deciding to download is the point: while the build is unsigned, that digest
// is the only integrity signal they have, and it was being thrown away.
function renderInstallerTrust(container: HTMLElement, update: ValidatedUpdate): void {
  const trust = describeInstallerTrust(update);
  container.replaceChildren();

  if (trust.warning) {
    const warning = document.createElement('p');
    warning.className = 'about-update-warning';
    warning.textContent = trust.warning;
    container.append(warning);
  }

  if (trust.verify) {
    const label = document.createElement('p');
    label.className = 'about-update-verify-label';
    label.textContent = '下载后请核对 SHA256：';

    const command = document.createElement('code');
    command.className = 'about-update-verify-command';
    command.textContent = trust.verify.command;

    const digest = document.createElement('code');
    digest.className = 'about-update-verify-digest';
    digest.textContent = trust.verify.sha256;

    container.append(label, command, digest);
  }
}

function setDialogOpen(dialog: HTMLDialogElement, open: boolean): void {
  if (open) {
    if (!dialog.open) {
      dialog.showModal();
    }
    return;
  }

  if (dialog.open) {
    dialog.close();
  }
}

export function setupAboutSettings(): void {
  setupToggleButton('serverDiagnosticLogToggleBtn', (active) => {
    updateConfig('general.diagnostic_log', active);
  });
  setupToggleButton('tsfDiagnosticLogToggleBtn', (active) => {
    updateConfig('general.tsf_diagnostic_log', active);
  });

  // These two rows shipped as buttons with no handler, so the settings page advertised a privacy
  // policy that did nothing when pressed. They are wired to the documents that actually govern the
  // software; the terms a user agrees to for an open-source product are its licence.
  for (const [id, url] of [
    ['about-website-link', WEBSITE_URL],
    ['about-license-link', LICENSE_URL],
    ['about-privacy-link', PRIVACY_URL],
  ] as const) {
    const button = document.getElementById(id);
    if (button instanceof HTMLButtonElement) {
      button.addEventListener('click', () => postExternalUrl(url));
    } else {
      console.warn(`[about] document link ${id} not found`);
    }
  }

  const checkButton = document.getElementById('about-check-update');
  const versionLabel = document.querySelector('#about-settings .about-version');
  const statusLabel = document.getElementById('about-update-status');
  const dialog = document.getElementById('about-update-dialog');
  const dialogVersion = document.getElementById('about-update-dialog-version');
  const dialogTrust = document.getElementById('about-update-dialog-trust');
  const cancelButton = document.getElementById('about-update-cancel');
  const downloadButton = document.getElementById('about-update-download');
  const mirrorButton = document.getElementById('about-update-mirror');

  if (!(checkButton instanceof HTMLButtonElement) ||
      !(versionLabel instanceof HTMLElement) ||
      !(statusLabel instanceof HTMLElement) ||
      !(dialog instanceof HTMLDialogElement) ||
      !(dialogVersion instanceof HTMLElement) ||
      !(dialogTrust instanceof HTMLElement) ||
      !(cancelButton instanceof HTMLButtonElement) ||
      !(downloadButton instanceof HTMLButtonElement) ||
      !(mirrorButton instanceof HTMLButtonElement)) {
    console.warn('[about] update controls not found');
    return;
  }

  let releaseUrl = RELEASES_PAGE_URL;
  let mirrorUrl: string | null = null;

  const setStatus = (message: string, kind: 'idle' | 'success' | 'error' = 'idle') => {
    statusLabel.textContent = message;
    statusLabel.dataset.kind = kind;
  };

  cancelButton.addEventListener('click', () => setDialogOpen(dialog, false));
  downloadButton.addEventListener('click', () => {
    setDialogOpen(dialog, false);
    postExternalUrl(releaseUrl);
  });
  mirrorButton.addEventListener('click', () => {
    if (!mirrorUrl) {
      return;
    }
    setDialogOpen(dialog, false);
    postExternalUrl(mirrorUrl);
  });
  dialog.addEventListener('click', (event) => {
    if (event.target === dialog) {
      setDialogOpen(dialog, false);
    }
  });

  checkButton.addEventListener('click', async () => {
    const currentVersion = parseVersion(versionLabel.textContent ?? '');
    if (!currentVersion) {
      setStatus('无法识别当前版本', 'error');
      return;
    }

    checkButton.disabled = true;
    checkButton.textContent = '正在检查…';
    setStatus('');

    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);

    try {
      const response = await fetch(`${UPDATE_MANIFEST_URL}?t=${Date.now()}`, {
        cache: 'no-store',
        signal: controller.signal
      });
      if (!response.ok) {
        throw new Error(`Update manifest returned ${response.status}`);
      }

      const manifest = await response.json() as UpdateManifest;
      const update = validateManifest(manifest, RELEASES_PAGE_URL);
      if (!update) {
        throw new Error('Invalid update manifest');
      }

      const latest = update.version;
      if (compareVersions(latest, currentVersion) > 0) {
        releaseUrl = update.releaseUrl;
        mirrorUrl = mirrorDownloadUrl(update, DOWNLOAD_MIRROR_PREFIX);
        mirrorButton.hidden = mirrorUrl === null;
        dialogVersion.textContent = `v${latest.display}`;
        renderInstallerTrust(dialogTrust, update);
        setStatus(`发现新版本 v${latest.display}`, 'success');
        setDialogOpen(dialog, true);
      } else {
        setStatus('已是最新版本', 'success');
      }
    } catch (error) {
      console.warn('[about] update check failed:', error);
      setStatus('检查失败，请稍后重试', 'error');
    } finally {
      window.clearTimeout(timeout);
      checkButton.disabled = false;
      checkButton.textContent = '检查更新';
    }
  });
}
