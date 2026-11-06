<!--
  PollCard.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  A poll in a message: the question, the options with their share of the
  votes. Clicking an option votes (single choice: clicking the chosen one
  again takes the vote back; multiple choice: toggles it).
-->
<script lang="ts">
  import { Check, ChartColumnBig } from '@lucide/svelte';
  import * as api from '../lib/api';
  import { store } from '../lib/store.svelte';
  import type { Message } from '../lib/types';

  let { m }: { m: Message } = $props();

  const poll = $derived(m.poll!);
  const canVote = $derived(!poll.closed && !m.pending);
  let busy = $state(false);

  function percent(votes: number): number {
    const sum = poll.options.reduce((n, o) => n + o.votes, 0);
    return sum ? Math.round((votes / sum) * 100) : 0;
  }

  function voterNames(ids: string[], own: boolean): string {
    const names = ids.map((id) => store.contacts[m.accountId]?.[id]?.name || id);
    if (own) names.unshift('You');
    return names.join(', ');
  }

  async function vote(id: string) {
    if (!canVote || busy) return;
    const chosen = poll.own.includes(id);
    const optionIds = poll.multiple
      ? (chosen ? poll.own.filter((o) => o !== id) : [...poll.own, id])
      : (chosen ? [] : [id]);
    busy = true;
    try {
      await api.call('messages.vote', { accountId: m.accountId, chatId: m.chatId, msgId: m.id, optionIds });
    } catch (e) {
      store.toastError(e);
    } finally {
      busy = false;
    }
  }

  const summary = $derived.by(() => {
    const parts = [poll.closed ? 'Final results' : poll.multiple ? 'Multiple choice' : 'Poll'];
    parts.push(poll.total === 1 ? '1 vote' : `${poll.total} votes`);
    return parts.join(' · ');
  });
</script>

<div class="poll" class:closed={poll.closed}>
  <div class="question"><ChartColumnBig size={16} strokeWidth={2} />{poll.question}</div>
  <div class="options" role="group" aria-label="Poll options">
    {#each poll.options as o (o.id)}
      {@const mine = poll.own.includes(o.id)}
      {@const share = percent(o.votes)}
      <button class="option" class:mine aria-pressed={mine} disabled={!canVote} onclick={() => vote(o.id)}
        title={o.voters.length || mine ? voterNames(o.voters, mine) : ''}>
        <span class="mark" class:round={!poll.multiple}>{#if mine}<Check size={12} strokeWidth={3} />{/if}</span>
        <span class="text">{o.text}</span>
        <span class="share">{o.votes ? `${share} %` : ''}</span>
        <span class="bar" style:width="{share}%"></span>
      </button>
    {/each}
  </div>
  <div class="summary">{summary}{#if poll.closed} · ended{/if}</div>
</div>

<style>
  .poll { display: flex; flex-direction: column; gap: 8px; min-width: min(280px, 60vw); padding: 2px 0; }
  .question { display: flex; align-items: flex-start; gap: 6px; font-weight: 600; line-height: 20px; overflow-wrap: anywhere; }
  .question :global(svg) { flex-shrink: 0; margin-top: 2px; color: var(--accent); }
  .options { display: flex; flex-direction: column; gap: 6px; }
  .option {
    position: relative; overflow: hidden; display: flex; align-items: center; gap: 8px; min-height: 34px;
    padding: 6px 10px; border: 1px solid var(--border-strong); border-radius: 10px; background: transparent;
    color: var(--text); text-align: left; font-size: 14px; cursor: pointer;
  }
  .option:disabled { cursor: default; }
  .option:not(:disabled):hover { border-color: var(--accent); }
  .option.mine { border-color: var(--accent); }
  .bar { position: absolute; left: 0; top: 0; bottom: 0; background: color-mix(in srgb, var(--accent) 16%, transparent); transition: width var(--duration-base, 160ms) ease; pointer-events: none; }
  .mark, .text, .share { position: relative; z-index: 1; }
  .mark { width: 16px; height: 16px; flex-shrink: 0; display: flex; align-items: center; justify-content: center; border: 1.5px solid var(--border-strong); border-radius: 4px; }
  .mark.round { border-radius: 50%; }
  .mine .mark { background: var(--accent); border-color: var(--accent); color: var(--on-accent); }
  .text { flex: 1; min-width: 0; overflow-wrap: anywhere; }
  .share { font-size: 12px; font-weight: 600; color: var(--text-2); font-variant-numeric: tabular-nums; }
  .summary { font-size: 12px; color: var(--text-muted); }
</style>
