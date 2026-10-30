<!--
  Qr.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.
-->
<script lang="ts">
  import QRCode from 'qrcode';

  let { data, size = 176 }: { data: string; size?: number } = $props();
  let svg = $state('');

  $effect(() => {
    // always dark on white, phones scan that reliably in both themes
    QRCode.toString(data, { type: 'svg', margin: 0, errorCorrectionLevel: 'M', color: { dark: '#1C1917', light: '#FFFFFF' } })
      .then((s) => (svg = s))
      .catch(() => (svg = ''));
  });
</script>

<div class="qr" style:width="{size}px" style:height="{size}px" role="img" aria-label="QR code">
  {@html svg}
</div>

<style>
  .qr { background: #fff; padding: 0; }
  .qr :global(svg) { width: 100%; height: 100%; display: block; }
</style>
