---
layout: page
title: PSP で X68000 日記
permalink: /psp/
---

{% assign series = site.data.series | where: "id", "psp" | first %}{{ series.description }}

{% include series-posts.html series="psp" %}
