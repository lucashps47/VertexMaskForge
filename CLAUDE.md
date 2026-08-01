# CLAUDE.md — Vertex Mask Forge

## Project

Plugin nativo Editor-only para Unreal Engine 5.8, para gerar e editar Vertex
Colors em Static Meshes.

- **Nome técnico / repositório:** VertexMaskForge
- **Nome visual (UI):** Vertex Mask Forge
- **Engine:** Unreal Engine 5.8
- **Projeto de teste:** `G:\UnrealProjects\MyProject`
- **Conexão com o projeto:** junction em
  `G:\UnrealProjects\MyProject\Plugins\VertexMaskForge` apontando para este
  repositório.

## Regras fundamentais

- Editor-only. Não deve compilar/carregar em builds de jogo (Runtime).
- Código 100% C++. Nenhum Blueprint.
- Interface 100% Slate. Nenhum Editor Utility Widget.
- Nenhuma dependência de Houdini ou Houdini Engine.
- Entrada de menu: `Tools → Custom Tools → Vertex Mask Forge`.
- Processamento de Vertex Color implementado nativamente em C++, usando as
  APIs de Dynamic Mesh e Geometry Script da UE 5.8 quando apropriado.

## Metodologia de desenvolvimento

- Desenvolvimento incremental, em checkpoints pequenos.
- Cada checkpoint deve compilar sem erros antes de seguir para o próximo.
- Nunca continuar implementando depois de um erro de build sem resolvê-lo
  primeiro.
- Antes de presumir a assinatura de uma API (Dynamic Mesh, Geometry Script,
  Slate, etc.), inspecionar o código-fonte real instalado da UE 5.8 em
  `G:\UE_5.8\Engine`. Não presumir com base em versões anteriores da engine.

## Architectural Documentation Protocol

Vertex Mask Forge possui documentação arquitetural viva e versionada junto do
plugin:

- [Docs/VertexMaskForgeArchitecture.md](Docs/VertexMaskForgeArchitecture.md)
  — descreve o estado arquitetural vigente (ownership, domínios, ciclo de
  vida de cores, composição, geradores, integração, limitações).
- [Docs/VertexMaskForgeDecisionLog.md](Docs/VertexMaskForgeDecisionLog.md)
  — registra as decisões arquiteturais aceitas, seus motivos, e as questões
  ainda em aberto.

### Início de todo checkpoint

Antes de auditar, planejar, implementar ou modificar qualquer parte do
Vertex Mask Forge:

1. Ler integralmente os dois documentos acima.
2. Usar `VertexMaskForgeArchitecture.md` como descrição do estado
   arquitetural vigente.
3. Usar `VertexMaskForgeDecisionLog.md` para entender decisões já aceitas,
   seus motivos, e o que ainda está em aberto (nunca tratar um item em
   aberto como decidido).
4. Depois da leitura, auditar no código e nos testes somente os arquivos,
   símbolos e fronteiras (boundaries) realmente afetados pelo checkpoint —
   não redescobrir a arquitetura inteira a cada vez.
5. Código e testes validados continuam sendo a autoridade factual: não
   confiar cegamente na documentação se ela divergir do que o código/teste
   realmente prova.
6. Se houver divergência entre documentação e código/testes, não seguir
   silenciosamente uma das duas versões: identificar a inconsistência,
   determinar o contrato realmente comprovado, corrigir a documentação
   dentro do escopo do checkpoint quando apropriado, e reportar a
   reconciliação.

### Durante o checkpoint

- Manter `Current Contract`, `Compatibility Boundary`, `Known Limitation` e
  `Planned` sempre claramente separados, como já definido nos dois
  documentos.
- Nunca apresentar comportamento `Planned` como se já estivesse
  implementado.
- Nunca tratar uma decisão ainda em aberto como `Accepted`.
- Tratar qualquer mudança de ownership, domínio, identidade, cardinalidade,
  composição, persistência ou integração como uma possível mudança
  arquitetural, mesmo que pequena.
- Relacionar novos testes ao boundary que eles congelam sempre que isso
  afetar a documentação.
- Manter a auditoria limitada à fronteira realmente afetada, depois da
  leitura inicial dos dois documentos.

### Revisão de impacto documental

Depois de implementar e validar, e antes de qualquer staging/commit, avaliar
explicitamente se o checkpoint mudou:

- o estado arquitetural vigente;
- ownership;
- o domínio render-vertex/source-topology;
- o comportamento de identidade (`MaskInstanceId`/`LayerId`);
- a composição ou a semântica de canais;
- o comportamento de geração/result-store;
- o contrato de cache/invalidação;
- o comportamento de preview, Accept, Cancel ou persistência;
- alguma compatibility boundary;
- o integration status;
- alguma limitação (introduzida, alterada ou resolvida);
- os testes representativos que protegem alguma fronteira;
- alguma decisão arquitetural (introduzida, revisada, superseded ou
  rejeitada);
- se, à luz do que foi avaliado acima, alguma atualização documental é
  necessária.

A revisão deve terminar em uma de duas saídas, nunca ambígua: atualizar
`VertexMaskForgeArchitecture.md` e/ou `VertexMaskForgeDecisionLog.md`
conforme os critérios da próxima seção, ou aplicar o protocolo de
"Checkpoints sem impacto documental".

### Quando atualizar cada documento

Atualizar `VertexMaskForgeArchitecture.md` quando: o estado arquitetural
vigente mudar; ownership, domain, identity, lifecycle ou data flow mudar; um
Current Contract ou Compatibility Boundary mudar; o integration status
mudar; uma limitação surgir, mudar ou for resolvida; os testes
representativos que protegem uma fronteira arquitetural mudarem; ou os
símbolos centrais/responsabilidades documentadas mudarem.

Atualizar `VertexMaskForgeDecisionLog.md` quando: uma decisão arquitetural
for introduzida; uma decisão `Accepted` for materialmente revisada; uma
decisão for superseded; uma decisão anterior deixar de representar o
projeto; ou uma questão listada em Future Decision Candidates se tornar uma
decisão comprovada.

Não criar um novo ADR para mudanças locais sem relevância arquitetural.

### Mesmo commit

Quando uma mudança de código ou teste alterar um contrato ou decisão
documentada:

- atualizar a documentação correspondente como parte do mesmo checkpoint;
- incluir código, testes e documentação no mesmo commit;
- nunca deixar a atualização documental para um checkpoint futuro;
- validar novamente a consistência entre implementação, testes, Architecture
  e Decision Log antes do commit.

Para metadata de baseline, seguir exatamente o protocolo definido em
`VertexMaskForgeArchitecture.md`, seção "Architectural Maintenance
Protocol". Nunca inventar ou antecipar o hash de um commit futuro.

### Checkpoints sem impacto documental

Quando a revisão de impacto documental concluir que nenhuma alteração é
necessária:

- não editar os documentos mecanicamente;
- declarar explicitamente no relatório final do checkpoint que os dois
  documentos foram lidos, que a revisão de impacto documental foi
  executada, que nenhuma mudança documental foi necessária, e apresentar
  uma justificativa curta baseada na fronteira (boundary) afetada pelo
  checkpoint.

Um checkpoint não deve ser considerado concluído sem essa declaração
explícita ou sem as atualizações documentais exigidas.

## Restrições

- Nunca editar arquivos da Engine (`G:\UE_5.8\Engine\...`).
- Nunca sobrescrever alterações preexistentes no projeto de teste ou no
  repositório sem inspecionar antes.
- Não adicionar assets binários ao repositório sem necessidade real.
- Nunca fazer commit sem autorização explícita do usuário.

## Ambiente

- Engine instalada em: `G:\UE_5.8`
- Projeto de teste: `G:\UnrealProjects\MyProject` (`EngineAssociation: 5.8`)
- O projeto de teste tem seu próprio repositório Git, independente deste.
  Não inicializar nem alterar Git dentro de `G:\UnrealProjects\MyProject`.
