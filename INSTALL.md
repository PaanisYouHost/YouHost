YouHost — ASENNUSOHJE
=====================

1. Lataa zip GitHub Actions -ajon sivulta. Artefakti on sivun alareunassa, kohdassa Artifacts. Nimi on YouHost-macOS-universal.

2. Pura zip kaksoisklikkauksella. YouHost.app ja tämä tiedosto (ASENNUSOHJE - INSTALL.txt) ovat vierekkäin. Jos ensimmäinen purku näyttää vain toisen zipin, pura sekin.

3. Vedä YouHost.app kansioon Applications (Ohjelmat).

4. Ensimmäisellä käynnistyksellä macOS estää allekirjoittamattoman ohjelman. Avaa Järjestelmäasetukset > Tietosuoja ja suojaus ja valitse Avaa silti (Open Anyway). Tai klikkaa YouHost.appia oikealla ja valitse Avaa. Vaihtoehto päätteessä:
   xattr -dr com.apple.quarantine /Applications/YouHost.app

5. Salli mikrofoni ja äänitulo, kun macOS kysyy. USB-äänikortti käyttää samaa lupaa.

6. Kytke äänikortti ennen käynnistystä. Käynnistysikkunassa valitaan laite, näytetaajuus, bittisyvyys, puskurin koko ja sessio.

7. Skannaa pluginit painikkeella SCAN.

8. Asetukset ja lokit ovat kansiossa ~/Library/Application Support/Ambient Audio/YouHost. Tiedostot ovat youhost.log ja crash-journal.txt.

9. Päivitys: sulje YouHost ja vaihda Applications-kansion YouHost.app uuteen. Sessiot pysyvät yhteensopivina.

10. Livessä suositeltu puskuri on 32-64. Älä skannaa plugineja keikan aikana.


YouHost — INSTALL
=================

1. Download the zip from the GitHub Actions run page. The artifact is at the bottom, under Artifacts. The name is YouHost-macOS-universal.

2. Unzip it (double-click). YouHost.app and this file (ASENNUSOHJE - INSTALL.txt) sit side by side. If the first unzip shows only another zip, unzip that one too.

3. Drag YouHost.app to Applications.

4. First launch: macOS blocks the unsigned app. Open System Settings > Privacy & Security and choose Open Anyway. Or right-click YouHost.app and choose Open. Alternative Terminal command:
   xattr -dr com.apple.quarantine /Applications/YouHost.app

5. Allow the microphone and audio input permission when macOS asks. A USB interface uses that same permission.

6. Connect the audio interface before launch. In the startup window, choose the device, sample rate, bit depth, buffer, and session.

7. Scan plugins with SCAN.

8. Settings and logs live in ~/Library/Application Support/Ambient Audio/YouHost. The files are youhost.log and crash-journal.txt.

9. Updating: quit YouHost, then replace YouHost.app in Applications. Sessions remain compatible.

10. The recommended buffer for live use is 32-64. Do not scan plugins during a show.
