# YouHost 1.0.18

2026-11-05

## Fixes

- A quit the user did not request is logged and refused. The close button, the Quit menu, and Command-Q still quit.
- Plugin editors open on the message thread. An Audio Unit view that arrives on another thread is attached on the message thread.
- The scanner runs one plugin per process. A timeout is recorded separately from a crash, and a failure is tried once more before a blacklist. A timeout is not blacklisted.
- The plugin list and the audio-card open run off the message thread.
- A callback whose length does not match the configured buffer is not counted as a dropout.
- Plugin protected mode added. A plugin is tested when it is first loaded and when its version changes. Only a crash or a quit runs it in a helper, with 32 samples of extra latency on that channel. Other plugins stay in-process.
- When a protected plugin crashes, that channel is muted and does not pass unprocessed audio. The plugin restarts with the same settings and the audio fades back in. OK only closes the notice. YouHost and the other channels, including recording, keep running.
- Plugin state is snapshotted off the audio thread a few seconds after parameter changes.
- Notices added: PLUGIN CRASHED, PLUGIN RESTORED, PLUGIN NOT RESTORED, and PLUGIN PROTECTED. A protected channel shows a shield.
- Session files stay format 7. A protect field is stored only for a protected slot. Older sessions still open.

## Korjaukset

- Lopetus, jota käyttäjä ei pyytänyt, kirjataan ja estetään. Sulje-painike, Quit-valikko ja Command-Q lopettavat edelleen.
- Plugin-ikkunat avataan viestisäikeellä. Audio Unit -näkymä, joka saapuu toiselta säikeeltä, liitetään viestisäikeellä.
- Skanneri ajaa yhden pluginin kerrallaan omassa prosessissaan. Aikakatkaisu kirjataan erikseen kaatumisesta, ja epäonnistuminen yritetään kerran uudelleen ennen estolistaa. Aikakatkaisua ei estetä.
- Pluginlista ja äänikortin avaus tehdään pois pääsäikeeltä.
- Takaisinkutsua, jonka pituus ei vastaa asetettua puskuria, ei lasketa dropoutiksi.
- Pluginin suojaustila lisätty. Plugin testataan ensilatauksessa ja kun sen versio muuttuu. Vain kaatuminen tai lopetus siirtää sen apuprosessiin, ja lisäviive on 32 näytettä sillä kanavalla. Muut pluginit pysyvät samassa prosessissa.
- Kun suojattu plugin kaatuu, kanava mykistetään eikä käsittelemätön ääni pääse läpi. Plugin käynnistyy samoilla asetuksilla ja ääni feidataan takaisin. OK vain sulkee ilmoituksen. YouHost ja muut kanavat, myös äänitys, jatkavat.
- Pluginin tila otetaan talteen pois audiosäikeeltä muutama sekunti parametrimuutoksen jälkeen.
- Ilmoitukset lisätty: PLUGIN CRASHED, PLUGIN RESTORED, PLUGIN NOT RESTORED ja PLUGIN PROTECTED. Suojatulla kanavalla näkyy kilpi.
- Sessiotiedostot pysyvät muodossa 7. protect-kenttä tallennetaan vain suojatulle slotille. Vanhat sessiot avautuvat edelleen.
