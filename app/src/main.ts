// main.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

import { mount } from 'svelte';
import App from './App.svelte';
import { store } from './lib/store.svelte';
import { theme } from './lib/theme.svelte';
import './styles/tokens.css';
import './styles/app.css';

theme.init();
const app = mount(App, { target: document.getElementById('app')! });
store.start();

// for the UI tests and debugging from the devtools console
(window as any).__braids = { store };

export default app;
