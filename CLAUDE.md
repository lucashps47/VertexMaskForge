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
