# Vertex Mask Forge

Plugin Editor-only para Unreal Engine 5.8 que permite gerar e editar Vertex
Colors em Static Meshes, com interface nativa em Slate.

## Arquitetura planejada

- Módulo de editor em C++ (sem módulo Runtime).
- Painel de ferramenta em Slate, acessível via `Tools → Custom Tools →
  Vertex Mask Forge`.
- Processamento de Vertex Color usando as APIs de Dynamic Mesh e Geometry
  Script da UE 5.8.
- Sem dependência de Houdini, Houdini Engine, Blueprint ou Editor Utility
  Widget.

## Estado atual

Setup inicial do repositório. Nenhum módulo, `.uplugin` ou interface ainda
foi implementado.

## Projeto de desenvolvimento

Este plugin é desenvolvido e testado a partir de:

`G:\UnrealProjects\MyProject`

## Conexão com o projeto de teste

Este repositório é conectado ao projeto de teste por meio de uma **junction
de diretório**, não por cópia de arquivos:

`G:\UnrealProjects\MyProject\Plugins\VertexMaskForge`
→ aponta para →
`G:\GIT\VertexMaskForge`
