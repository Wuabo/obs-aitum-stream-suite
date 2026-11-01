# Aitum Stream Suite

Plugin para o OBS Studio. As docks ficam em **Docks**:

- **Outputs** — cada saída (stream ou gravação) escolhe o canvas, o servidor e a chave. No canvas principal, com o stream ou a gravação nativa do OBS já a correr, o vídeo usa o encoder do OBS. Nos outros canvas cria um encoder novo com o **mesmo tipo e as mesmas definições** de Definições → Saída (encoder, bitrate, preset, qualidade). Não fica preso a 6000 kbps. Se a saída tiver **Custom Encoder Settings** ligado e um encoder escolhido, usa essas definições em vez das do OBS.
- **Stream Info** — título, categoria e tags na conta ligada.
- **Chat** — chat unificado das saídas configuradas. Moderação (apagar, banir, timeout) na Twitch, no YouTube e na Kick. Slow mode, só seguidores, só subscritores e só emojis existem na Twitch; a API pública da Kick e a Data API do YouTube não expõem esses interruptores.
- **Activity Feed** — follow, sub, gift, bits, raid, resgate, Super Chat e membro. A Twitch chega pelo EventSub mesmo com o canal offline, desde que a conta esteja ligada. A Kick chega pela ligação do chat. O YouTube (Super Chat e membro) chega pelo chat da live. No topo da dock filtras por plataforma e podes limpar o histórico.

O Client ID e o client secret não ficam na dock. No fundo da dock **Stream Info** há um botão de definições. Aí colas as credenciais. Os campos começam ocultos; o botão à direita mostra ou esconde o valor.

O OBS grava isso no `aitum.json` do perfil atual, em `%APPDATA%\obs-studio\basic\profiles\<perfil>\aitum.json`. O client secret e os tokens ficam cifrados com a conta do Windows. Não é preciso editar esse ficheiro à mão.

Redirect usado pelo plugin no YouTube e na Kick, exatamente assim:

`http://127.0.0.1:17654/callback`

## Twitch

1. Abre https://dev.twitch.tv/console/apps e cria uma aplicação.
2. Em OAuth Redirect URL podes usar `http://localhost`. O login da Twitch neste plugin é por código de dispositivo, não por esse redirect.
3. Em **Category**, escolhe **Broadcaster Suite**.
4. Em **Client Type**, escolhe **Public**. Este plugin não usa client secret.
5. Copia o **Client ID**.
6. Na dock, abre as definições e cola-o no campo Twitch.

Permissões pedidas no login. Marca estas na app, se a consola as mostrar, e volta a **Ligar** no OBS se a conta já estava ligada com menos permissões:

- `channel:manage:broadcast` — gerir a transmissão (título, categoria, tags)
- `chat:read` — ler o chat
- `chat:edit` — escrever no chat
- `moderator:manage:chat_messages` — apagar mensagens
- `moderator:manage:banned_users` — banir, timeout e desbanir
- `moderator:manage:chat_settings` — slow mode, só seguidores, só subscritores, só emojis
- `moderator:read:followers` — follows no Activity Feed
- `channel:read:subscriptions` — subscrições
- `bits:read` — bits
- `channel:read:redemptions` — resgates de pontos do canal
- `channel:moderate` — bans e unbans que chegam ao chat

## YouTube

Nomes conforme a Consola Google atual (**Google Auth Platform** / **APIs e serviços**).

1. Abre https://console.cloud.google.com/ e cria/seleciona o projeto.
2. Em **APIs e serviços → Biblioteca**, procura e ativa a **YouTube Data API v3**. Tem de aparecer em **APIs e serviços ativados**.
3. Em **Google Auth Platform → Branding**, preenche o nome da app e o e-mail de suporte.
4. Em **Google Auth Platform → Público-alvo**:
   - Tipo de utilizador: **Externo** (se não tiveres Google Workspace).
   - Estado: **Teste** (normal até a Google validar a app).
   - Em **Utilizadores de teste**, carrega em **Adicionar utilizadores** e cola a conta Google do teu canal YouTube. Sem isto o browser mostra `Erro 403: access_denied` / “só pode ser acedida por testadores”.
5. Em **Google Auth Platform → Acesso a dados → Adicionar ou remover escopos**, filtra por **YouTube Data API v3** e marca só este:
   - `https://www.googleapis.com/auth/youtube` — **Gerenciar sua conta do YouTube**
   Não marques `youtube.readonly`, `youtube.upload`, `youtube.force-ssl` nem os outros.
6. Em **Google Auth Platform → Clientes** (ou **APIs e serviços → Credenciais**), cria um **ID do cliente OAuth**.
7. Em **Tipo de aplicação**, escolhe **Aplicação Web**.
8. Em **URIs de redirecionamento autorizados**, adiciona `http://127.0.0.1:17654/callback`.
9. Copia o **ID do cliente** e o **Segredo do cliente**.
10. Na dock, abre as definições e cola-os nos campos YouTube.

A dock atualiza a transmissão ativa ou a próxima agendada. O chat da live lê a página do YouTube; não há chave de API no código do plugin.

Para o erro de testadores: vai a **Google Auth Platform → Público-alvo → Utilizadores de teste**, adiciona a tua conta, guarda, e volta a **Ligar** no OBS.

Se aparecer **exceeded your quota**: o projeto Google tem um limite diário (por defeito ~10 000 unidades na YouTube Data API v3). Cada poll do chat e cada Apply gastam quota. Vê o uso em **APIs e serviços → Quotas**. A quota renova à meia-noite (hora do Pacífico). Evita spam de Apply / reopen do dock enquanto estiveres no limite.

## Kick

1. Abre as definições de developer da Kick (https://kick.com/settings/developer) e cria uma app.
2. No redirect, cola `http://127.0.0.1:17654/callback`.
3. Inclui estes scopes (nomes da consola Kick):
   - Ler informações do usuário (incluindo endereço de e-mail) — `user:read`
   - Ler informações do canal — `channel:read`
   - Atualizar informações do canal — `channel:write`
   - Escrever no feed do Chat — `chat:write`
   - Executar ações de moderação nas mensagens do chat — `moderation:chat_message:manage`
   - Executar ações de moderação para moderadores — `moderation:ban`
   Não marques os outros (stream key, eventos, recompensas, anúncios, KICKs).
4. Copia o **Client ID** e o **client secret**.
5. Na dock, abre as definições e cola-os nos campos Kick.

A API pública da Kick aceita título e categoria. Tags podem ser recusadas; nesse caso o plugin aplica só título e categoria.

## Depois de guardar

Escolhe a plataforma na dock e carrega em **Ligar**. O login abre no browser. Na Twitch, o OBS mostra um código para introduzires em https://www.twitch.tv/activate.

Fecha o OBS por completo antes de substituir a DLL. Um reload não chega: o processo tem de sair.
