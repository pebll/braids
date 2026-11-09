<!--
  PollCard.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  A poll in a message: the question, the options with their share of the
  votes: the leading option's bar is full, each shows its count and up to
  five voters' pictures (+N for more). Clicking an option votes (single choice: clicking the chosen one
  again takes the vote back; multiple choice: toggles it).
-->
<script lang="ts">
  import { Check, ChartColumnBig } from '@lucide/svelte';
  import * as api from '../lib/api';
  import { store } from '../lib/store.svelte';
  import type { Message } from '../lib/types';
  import Avatar from './Avatar.svelte';

  let { m }: { m: Message } = $props();

  const poll = $derived(m.poll!);
  const canVote = $derived(!poll.closed && !m.pending);
  let busy = $state(false);

  const MAX_FACES = 5;
  // bars relative to the leading option, which is full
  const most = $derived(Math.max(0, ...poll.options.map((o) => o.votes)));
  const selfId = $derived(store.accounts[m.accountId]?.selfId ?? '');

  function width(votes: number): number {
    return most ? Math.round((votes / most) * 100) : 0;
  }

  // the own vote: your name and picture from this account, else from another
  // of your accounts that has them (not every messenger tells or has one)
  const me = $derived.by(() => {
    const accounts = [store.accounts[m.accountId], ...store.accountList.filter((a) => a.accountId !== m.accountId)]
      .filter((a) => a?.selfId);
    const named = (a: (typeof accounts)[number]) => {
      const n = store.contacts[a.accountId]?.[a.selfId]?.name;
      return n && n !== 'You' ? n : '';
    };
    const pictured = accounts.find((a) => store.avatar(a.accountId, a.selfId));
    const withName = accounts.find((a) => named(a));
    const face = pictured ?? accounts[0];
    return {
      owner: face?.accountId ?? m.accountId,
      id: face?.selfId ?? selfId,
      name: (withName && named(withName)) || 'You',
    };
  });

  function name(id: string): string {
    return id === selfId ? me.name : store.contacts[m.accountId]?.[id]?.name || id;
  }

  // the voters known by name (Telegram only counts), own vote first
  function voters(ids: string[], own: boolean): string[] {
    const list = ids.filter((id) => id !== selfId);
    return own ? [selfId, ...list] : list;
  }

  function title(ids: string[], votes: number): string {
    if (!votes) return '';
    const names = ids.map(name).join(', ');
    return `${votes === 1 ? '1 vote' : `${votes} votes`}${names ? `: ${names}` : ''}`;
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
      {@const who = voters(o.voters, mine)}
      <button class="option" class:mine class:leading={o.votes > 0 && o.votes === most} aria-pressed={mine} disabled={!canVote}
        onclick={() => vote(o.id)} title={title(who, o.votes)}>
        <span class="mark" class:round={!poll.multiple}>{#if mine}<Check size={12} strokeWidth={3} />{/if}</span>
        <span class="text">{o.text}</span>
        {#if who.length}
          <span class="faces" aria-hidden="true">
            {#each who.slice(0, MAX_FACES) as id, i (i)}
              {#if id === selfId}
                <span class="face" title={me.name}><Avatar name={me.name} id={me.id} size={18} owner={me.owner} /></span>
              {:else}
                <span class="face" title={name(id)}><Avatar name={name(id)} {id} size={18} owner={m.accountId} /></span>
              {/if}
            {/each}
            {#if who.length > MAX_FACES}<span class="more">+{who.length - MAX_FACES}</span>{/if}
          </span>
        {/if}
        <span class="votes">{o.votes || ''}</span>
        <span class="bar" style:width="{width(o.votes)}%"></span>
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
  .mark, .text, .votes, .faces { position: relative; z-index: 1; }
  .mark { width: 16px; height: 16px; flex-shrink: 0; display: flex; align-items: center; justify-content: center; border: 1.5px solid var(--border-strong); border-radius: 4px; }
  .mark.round { border-radius: 50%; }
  .mine .mark { background: var(--accent); border-color: var(--accent); color: var(--on-accent); }
  .text { flex: 1; min-width: 0; overflow-wrap: anywhere; }
  .votes { min-width: 1ch; text-align: right; font-size: 13px; font-weight: 600; color: var(--text-2); font-variant-numeric: tabular-nums; }
  .leading .votes { color: var(--text); }
  .faces { display: flex; align-items: center; flex-shrink: 0; }
  .face { display: flex; border-radius: 50%; box-shadow: 0 0 0 1.5px var(--bg-1); }
  .face + .face { margin-left: -5px; }
  .more { margin-left: 3px; font-size: 11px; font-weight: 600; color: var(--text-muted); }
  .summary { font-size: 12px; color: var(--text-muted); }
</style>
