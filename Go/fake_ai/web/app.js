const log = document.getElementById('log');
const form = document.getElementById('form');
const box = document.getElementById('box');
const chips = document.getElementById('chips');

const suggestions = [
  '我今天好累，不想写作业',
  '我最近很焦虑',
  '我明天不想起床',
  '我晚上睡不着',
  '王博韬是谁？',
  '去哪能和世界上最帅的聊天？'
];

function addMsg(role, text) {
  const bubble = document.createElement('div');
  bubble.className = 'msg ' + role;
  bubble.textContent = text;
  log.appendChild(bubble);
  log.scrollTop = log.scrollHeight;
  return bubble;
}

async function send(text) {
  text = (text || '').trim();
  if (!text) return;
  addMsg('user', text);
  box.value = '';
  const bubble = addMsg('bot', '……');
  try {
    const res = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ message: text })
    });
    const data = await res.json();
    setTimeout(() => {
      bubble.textContent = data.reply;
      log.scrollTop = log.scrollHeight;
    }, 420);
  } catch (e) {
    bubble.textContent = '（它真的宕机了，刷新试试。）';
  }
}

form.addEventListener('submit', (ev) => {
  ev.preventDefault();
  send(box.value);
});

suggestions.forEach((s) => {
  const b = document.createElement('button');
  b.type = 'button';
  b.className = 'chip';
  b.textContent = s;
  b.addEventListener('click', () => send(s));
  chips.appendChild(b);
});

addMsg('bot', '你好，我是 Fake AI。慢慢说，token 用不完。');
