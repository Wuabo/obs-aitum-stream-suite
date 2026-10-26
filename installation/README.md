# Apps das plataformas

A dock **Info da transmissão** altera título, categoria e tags na conta ligada. Cada plataforma exige uma app criada por ti.

O Client ID e o client secret não ficam na dock. No fundo da dock há um botão de definições, igual ao das saídas. Aí colas as credenciais. Os campos começam ocultos; o botão à direita mostra ou esconde o valor.

O OBS grava isso no `aitum.json` do perfil atual, em `%APPDATA%\obs-studio\basic\profiles\<perfil>\aitum.json`. Não é preciso editar esse ficheiro à mão.

Redirect usado pelo plugin no YouTube e na Kick, exatamente assim:

`http://127.0.0.1:17654/callback`

## Twitch

1. Abre https://dev.twitch.tv/console/apps e cria uma aplicação.
2. Em OAuth Redirect URL podes usar `http://localhost`. O login da Twitch neste plugin é por código de dispositivo, não por esse redirect.
3. Em **Category**, escolhe **Broadcaster Suite**.
4. Em **Client Type**, escolhe **Public**. Este plugin não usa client secret.
5. Copia o **Client ID**.
6. Na dock, abre as definições e cola-o no campo Twitch.

Permissões pedidas no login: `channel:manage:broadcast`, `chat:read`, `chat:edit` (título/categoria + chat unificado).

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

A dock atualiza a transmissão ativa ou a próxima agendada.

Para o erro de testadores: vai a **Google Auth Platform → Público-alvo → Utilizadores de teste**, adiciona a tua conta, guarda, e volta a **Ligar** no OBS.

Se aparecer **exceeded your quota**: o projeto Google tem um limite diário (por defeito ~10 000 unidades na YouTube Data API v3). Cada poll do chat e cada Apply gastam quota. Vê o uso em **APIs e serviços → Quotas**. A quota renova à meia-noite (hora do Pacífico). Evita spam de Apply / reopen do dock enquanto estiveres no limite.

## Kick

1. Abre as definições de developer da Kick (https://kick.com/settings/developer) e cria uma app.
2. No redirect, cola `http://127.0.0.1:17654/callback`.
3. Inclui os scopes `user:read`, `channel:read`, `channel:write` e `chat:write`.
4. Copia o **Client ID** e o **client secret**.
5. Na dock, abre as definições e cola-os nos campos Kick.

A API pública da Kick aceita título e categoria. Tags podem ser recusadas; nesse caso o plugin aplica só título e categoria.

## Depois de guardar

Escolhe a plataforma na dock e carrega em **Ligar**. O login abre no browser. Na Twitch, o OBS mostra um código para introduzires em https://www.twitch.tv/activate.
