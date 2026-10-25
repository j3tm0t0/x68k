---
layout: page
title: X68000 on a PSP Diary
permalink: /en/psp/
---

{% assign series = site.data.series | where: "id", "psp" | first %}{{ series.description_en }}

{% include series-posts.html series="psp" lang="en" %}
