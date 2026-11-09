---
layout: page
title: Cannonball for x68 日記
permalink: /cannonball/
---

{% assign series = site.data.series | where: "id", "cannonball" | first %}{{ series.description }}

{% include series-posts.html series="cannonball" %}
