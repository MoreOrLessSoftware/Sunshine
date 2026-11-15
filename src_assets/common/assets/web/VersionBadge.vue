<template>
  <div class="version-badge" v-if="displayVersion">
    <a class="badge text-bg-warning version-update-badge"
       v-if="update"
       :href="update.url"
       target="_blank"
       rel="noopener noreferrer"
       :title="update.name">
      <download :size="14" class="icon"></download>
      {{ $t(update.preRelease ? 'index.pre_release_available' : 'index.update_available', {version: update.version}) }}
    </a>
    <span class="text-body-secondary version-badge-text">{{ $t('index.version', {version: displayVersion}) }}</span>
  </div>
</template>

<script>
import { Download } from '@lucide/vue'
import SunshineVersion from './sunshine_version'

/**
 * The installed Sunshine version, shown at the end of a page header, with a link to a newer release when there is one.
 * Emits `version-info` with `{installedVersionNotStable}` once the release check is done.
 */
export default {
  components: {
    Download,
  },
  emits: ['version-info'],
  data() {
    return {
      displayVersion: null,
      notifyPreReleases: false,
      preReleaseVersion: null,
      stableVersion: null,
      version: null,
    }
  },
  computed: {
    /**
     * The newer release to offer, preferring a stable release over a pre-release, or `null` when up to date.
     *
     * @returns {object|null} The release's `version`, `name`, `url` and whether it's a `preRelease`.
     */
    update() {
      if (!this.version) {
        return null;
      }
      const describe = (release, preRelease) => ({
        name: release.release.name,
        preRelease,
        url: release.release.html_url,
        version: release.version,
      });
      if (this.stableVersion && this.stableVersion.isGreater(this.version)) {
        return describe(this.stableVersion, false);
      }
      if (this.notifyPreReleases && this.preReleaseVersion && this.preReleaseVersion.isGreater(this.version) &&
          (!this.stableVersion || this.preReleaseVersion.isGreater(this.stableVersion))) {
        return describe(this.preReleaseVersion, true);
      }
      return null;
    },
  },
  async created() {
    let config;
    try {
      config = await fetch('./api/config').then((r) => r.json());
      this.version = new SunshineVersion(null, config.version);
      this.displayVersion = config.display_version || config.version;
      this.notifyPreReleases = config.notify_pre_releases;
    } catch (e) {
      console.error('Failed to load the Sunshine version:', e);
      return;
    }

    // Builds can leave out the check for newer releases, such as a custom fork's
    if (config.update_check === false) {
      return;
    }
    try {
      const releases = await fetch('https://api.github.com/repos/LizardByte/Sunshine/releases').then((r) => r.json());
      const stable = releases.find((release) => !release.prerelease && !release.draft);
      const preRelease = releases.find((release) => release.prerelease);
      this.stableVersion = stable ? new SunshineVersion(stable, null) : null;
      this.preReleaseVersion = preRelease ? new SunshineVersion(preRelease, null) : null;
    } catch (e) {
      console.error('Failed to check for a newer Sunshine release:', e);
    }
    this.$emit('version-info', {
      installedVersionNotStable: !!(this.stableVersion && this.version.isGreater(this.stableVersion)),
    });
  },
}
</script>
