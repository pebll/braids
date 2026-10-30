<!--
  Logo.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.
-->
<script lang="ts">
  import { theme } from '../lib/theme.svelte';

  let { size = 24 }: { size?: number } = $props();
</script>

<img src={theme.dark ? '/braids-mark-dark.svg' : '/braids-mark.svg'} width={size} height={size} alt="braids" draggable="false" />
