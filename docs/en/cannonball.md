---
layout: page
title: Cannonball for x68 Diary
permalink: /en/cannonball/
---

{% assign series = site.data.series | where: "id", "cannonball" | first %}{{ series.description_en }}

{% include series-posts.html series="cannonball" lang="en" %}
