/**
 * @file Featured apps that this fork lists ahead of the LizardByte app directory.
 */

/**
 * The category the LizardByte app directory uses for streaming clients.
 */
export const CLIENT_CATEGORY = {
  id: 'sunshine:client',
  name: 'Streaming Clients',
  originalId: 'client',
};

/**
 * The companion clients for this fork, shown first on the Featured Apps page in this order.
 *
 * Entries use the LizardByte app directory schema, plus `pinned` (keep at the top, in list order)
 * and `githubRepo` (the `owner/name` whose live stats are fetched from the GitHub API).
 */
export const forkFeaturedApps = [
  {
    id: 'mlsoft-moonlight-qt',
    name: 'Moonlight Experimental',
    tagline: 'MoreOrLessSoftware Moonlight PC',
    description: 'The companion PC client for this host: an overlay menu with host commands, video presets, '
      + 'host-synced frame pacing, AMF decoding on AMD, PyroWave, and detailed latency stats.',
    category: CLIENT_CATEGORY.id,
    platforms: ['windows', 'linux'],
    icon: 'https://github.com/MoreOrLessSoftware.png',
    screenshots: [
      'https://raw.githubusercontent.com/MoreOrLessSoftware/moonlight-qt/mlsoft-custom2/screenshots/v1.0.0/overlay_menu.webp',
      'https://raw.githubusercontent.com/MoreOrLessSoftware/moonlight-qt/mlsoft-custom2/screenshots/v1.0.4/settings.webp',
      'https://raw.githubusercontent.com/MoreOrLessSoftware/moonlight-qt/mlsoft-custom2/screenshots/v1.0.0/stats.webp',
      'https://raw.githubusercontent.com/MoreOrLessSoftware/moonlight-qt/mlsoft-custom2/screenshots/v1.1.0/input_settings.webp',
    ],
    links: {
      github: 'https://github.com/MoreOrLessSoftware/moonlight-qt',
      download: 'https://github.com/MoreOrLessSoftware/moonlight-qt/releases/latest',
    },
    downloads: [
      {
        label: 'Download on GitHub',
        url: 'https://github.com/MoreOrLessSoftware/moonlight-qt/releases/latest',
      },
    ],
    pinned: true,
    githubRepo: 'MoreOrLessSoftware/moonlight-qt',
  },
  {
    id: 'mlsoft-moonlight-android',
    name: 'Moonlight X',
    tagline: 'MoreOrLessSoftware Moonlight for Android',
    description: 'The companion Android and Android TV client for this host: Quick Launch apps, per-app settings, '
      + 'an overlay menu with host commands, a Vulkan renderer with host-synced frame pacing, and PyroWave. '
      + 'Installs next to the original Moonlight.',
    category: CLIENT_CATEGORY.id,
    platforms: ['android', 'tv'],
    icon: 'https://github.com/MoreOrLessSoftware.png',
    screenshots: [
      'https://raw.githubusercontent.com/MoreOrLessSoftware/moonlight-android/mlsoft-x/screenshots/v0.8.0/apps_view.png',
      'https://raw.githubusercontent.com/MoreOrLessSoftware/moonlight-android/mlsoft-x/screenshots/v0.8.0/overlay_menu.png',
      'https://raw.githubusercontent.com/MoreOrLessSoftware/moonlight-android/mlsoft-x/screenshots/v0.8.0/settings.png',
      'https://raw.githubusercontent.com/MoreOrLessSoftware/moonlight-android/mlsoft-x/screenshots/v0.8.0/stats.png',
    ],
    links: {
      github: 'https://github.com/MoreOrLessSoftware/moonlight-android',
      download: 'https://github.com/MoreOrLessSoftware/moonlight-android/releases/latest',
    },
    downloads: [
      {
        label: 'Download on GitHub',
        url: 'https://github.com/MoreOrLessSoftware/moonlight-android/releases/latest',
      },
    ],
    pinned: true,
    githubRepo: 'MoreOrLessSoftware/moonlight-android',
  },
];

/**
 * Put the local apps ahead of the remote ones, dropping any remote app with the same id.
 *
 * @param {Object[]} remoteApps Apps from the LizardByte app directory.
 * @param {Object[]} localApps Apps listed by this fork.
 * @returns {Object[]} The local apps followed by the remaining remote apps.
 */
export function mergeFeaturedApps(remoteApps, localApps) {
  const localIds = new Set(localApps.map((app) => app.id));
  return [...localApps, ...remoteApps.filter((app) => !localIds.has(app.id))];
}

/**
 * Add the categories the local apps need when the remote list did not provide them.
 *
 * @param {Object[]} remoteCategories Categories from the LizardByte app directory.
 * @param {Object[]} localApps Apps listed by this fork.
 * @returns {Object[]} The remote categories, plus the client category if it is missing and used.
 */
export function mergeFeaturedCategories(remoteCategories, localApps) {
  const ids = new Set(remoteCategories.map((category) => category.id));
  const needsClient = localApps.some((app) => app.category === CLIENT_CATEGORY.id);
  return needsClient && !ids.has(CLIENT_CATEGORY.id) ? [CLIENT_CATEGORY, ...remoteCategories] : remoteCategories;
}

/**
 * Compare two featured apps for display order: pinned apps first in their list order,
 * then official apps, then the rest by GitHub stars.
 *
 * @param {Object} a The first app, with `_order` set to its position in the merged list.
 * @param {Object} b The second app, with `_order` set to its position in the merged list.
 * @returns {number} A negative number if `a` comes first, positive if `b` does, otherwise 0.
 */
export function compareFeaturedApps(a, b) {
  if (a.pinned || b.pinned) {
    if (a.pinned && b.pinned) return a._order - b._order;
    return a.pinned ? -1 : 1;
  }
  if (a.official !== b.official) return a.official ? -1 : 1;
  return (b.github?.stars || 0) - (a.github?.stars || 0);
}

/**
 * Fetch a repository's stars, forks, open issues and last push time from the GitHub API.
 *
 * @param {string} repo The repository as `owner/name`.
 * @returns {Promise<Object|null>} Stats in the app directory's `github` shape, or null on any failure.
 */
export async function fetchGithubStats(repo) {
  try {
    const response = await fetch(`https://api.github.com/repos/${repo}`);
    if (!response.ok) return null;
    const data = await response.json();
    return {
      stars: data.stargazers_count,
      forks: data.forks_count,
      openIssues: data.open_issues_count,
      lastUpdated: data.pushed_at,
      license: data.license?.spdx_id,
    };
  } catch (err) {
    console.warn(`Could not load GitHub stats for ${repo}:`, err);
    return null;
  }
}
