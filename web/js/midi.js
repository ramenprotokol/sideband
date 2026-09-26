// Optional Web MIDI input. Only asked for when the visitor presses
// CONNECT MIDI, so nobody sees a permission prompt they did not ask for.

// Turns one MIDI message into a synth action, or null. Exported for tests.
export function parseMidi(data) {
  if (!data || data.length < 2) return null;
  const status = data[0] & 0xf0;
  const d1 = data[1] & 0x7f;
  const d2 = data.length > 2 ? data[2] & 0x7f : 0;
  if (status === 0x90 && data.length > 2) return d2 > 0 ? { type: 'on', note: d1, velocity: d2 } : { type: 'off', note: d1 };
  if (status === 0x80) return { type: 'off', note: d1 };
  if (status === 0xb0 && (d1 === 120 || d1 === 123)) return { type: 'allOff' };
  return null;
}

export async function connectMidi({ onMessage, onStatus }) {
  if (typeof navigator.requestMIDIAccess !== 'function') {
    onStatus('This browser does not offer Web MIDI. The computer keyboard and the on-screen keys still play.');
    return null;
  }
  let access;
  try {
    access = await navigator.requestMIDIAccess({ sysex: false });
  } catch (e) {
    const denied = e && (e.name === 'SecurityError' || e.name === 'NotAllowedError');
    onStatus(denied
      ? 'MIDI access was declined, so sideband will not listen to MIDI. The computer keyboard still plays.'
      : `MIDI could not start (${(e && e.message) || 'unknown error'}). The computer keyboard still plays.`);
    return null;
  }
  const listen = () => {
    let count = 0;
    for (const input of access.inputs.values()) {
      input.onmidimessage = (ev) => {
        const action = parseMidi(ev.data);
        if (action) onMessage(action);
      };
      count++;
    }
    onStatus(count
      ? `MIDI connected: listening to ${count} input${count === 1 ? '' : 's'} on all channels.`
      : 'MIDI is allowed, but no MIDI input is plugged in yet. Plug one in and it will be picked up.');
  };
  access.onstatechange = listen;
  listen();
  return access;
}
