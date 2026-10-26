"use strict";
(function(){
  // ---- Format constants (verified against funcs_2.c / funcs_17.c) ----
  var FILE_SIZE=0x200, HEADER=0x20, REC_SIZE=0xC8, BODY_OFF=0x18, BODY_SIZE=0xB0, UNK08_OFF=0x08, UNK08_SIZE=0x10;
  var REC=[0x20,0xE8];                 // two body copies
  var N_LEVELS_MEDAL=19, N_ACCTS=3, N_ELITE=10;
  var MEDAL_NAMES=["none","brz","slv","gld"], MEDAL_WORDS=["no medal","bronze","silver","gold"];
  // Medal slot / level number order (0-15 the campaign, 16-18 the bonus levels).
  var LEVEL_NAMES=["Ambush at Mos Eisley","Rendezvous on Barkhesh","The Search for the Nonnah","Defection at Corellia","Liberation of Gerrard V",
    "The Jade Moon","Imperial Construction Yards","Assault on Kile II","Rescue on Kessel","Prisons of Kessel","Battle above Taloraan",
    "Escape from Fest","Blockade on Chandrila","Raid on Sullust","Moff Seerdon's Revenge","The Battle of Calamari","Beggar's Canyon",
    "The Death Star Trench Run","Battle of Hoth"];
  function levelLabel(l){ return "Level "+(l+1)+": "+(LEVEL_NAMES[l]||"?"); }
  function attr(s){ return String(s).replace(/&/g,"&amp;").replace(/"/g,"&quot;"); }
  var RANKS=["Trainee","Cadet","Ensign","Officer","Lieutenant","Flight Leader","Captain","Squad Leader",
             "Gold Leader","Major","Commander","Colonel","General","Line Admiral","Fleet Admiral","Supreme Allied Cmdr"];
  // Per-account weapon upgrades: account_data +0x0C "unlock word", mask 0x7FFE00.
  var UPGRADE_BITS=[
    {bit:0x00000200,name:"Adv. lasers",desc:"Advanced lasers: the stronger blaster power-up, on every craft with lasers"},
    {bit:0x00000400,name:"Adv. missiles",desc:"Advanced concussion missiles: the stronger missile power-up"},
    {bit:0x00000800,name:"Seeker missiles",desc:"Seeker concussion missiles: missiles that home in on a target"},
    {bit:0x00001000,name:"Adv. torpedoes",desc:"Advanced proton torpedoes: the stronger torpedo power-up"},
    {bit:0x00002000,name:"Seeker torpedoes",desc:"Seeker proton torpedoes: torpedoes that home in on a target"},
    {bit:0x00004000,name:"Seeker cluster",desc:"Seeker cluster missiles: a spread of homing missiles"},
    {bit:0x00008000,name:"Adv. bombs",desc:"Advanced bombs: the stronger bomb power-up"}
  ];
  // Cheat flags -> save cheatCodeFlags[0] (body +0x08). Bit map confirmed in cheat_codes.md.
  // The Naboo Starfighter has no flag: typing !YNGWIE! after HALIFAX? saves the key that decrypts its model (the CRC of
  // !YNGWIE!) at body +0xAC, and the game loads the ship only with that key.
  var NABOO_KEY=0x050FE09C;
  var CHEAT_GROUPS=[
    {title:"Ships & craft",items:[
      {bit:0x00000004,name:"Millennium Falcon",code:"FARMBOY"},
      {bit:0x00000008,name:"TIE Fighter",code:"TIEDUP"},
      {naboo:true,name:"Naboo Starfighter",code:"HALIFAX? !YNGWIE!",note:"Same as typing HALIFAX? and then !YNGWIE! on the passcode screen: the game saves the ship's unlock key, so the Naboo Starfighter stays unlocked for every pilot"},
      {bit:0x00000002,name:"Buick V-Wing",code:"KOELSCH",note:"Same as typing KOELSCH on the passcode screen: the V-wing is replaced by a 1969 Buick convertible for every pilot"},
      {bit:0x02000000,name:"Play as AT-ST",code:"CHICKEN",disabled:true,note:"Passcode screen only: CHICKEN takes effect when typed there"}]},
    {title:"Bonus levels",items:[
      {bit:0x04000000,name:"Tatooine race",code:"WOMPRAT!"},
      {bit:0x08000000,name:"Death Star trench",code:"WOISTHAN"},
      {bit:0x10000000,name:"Battle of Hoth",code:"DEADDACK"}]},
    {title:"Gameplay",items:[
      {bit:0x00000001,name:"All missions unlocked",code:"GAMEFLO!"},
      {bit:0x01000000,name:"All weapon & shield powerups",code:"TOUGHGUY"},
      {bit:0x00400000,name:"Unlimited lives",code:"IGIVEUP"},
      {bit:0x00000040,name:"Hard difficulty",code:"ACE"},
      {bit:0x00800000,name:"Altitude radar",code:"RADAR"}]},
    {title:"Menus & extras",items:[
      {bit:0x00080000,name:"Music menu",code:"MAESTRO"},
      {bit:0x00100000,name:"Cutscene viewer",code:"DIRECTOR"},
      {bit:0x00200000,name:"Credits",code:"CREDITS",disabled:true,note:"Passcode screen only: typing CREDITS there plays the credits"},
      {bit:0x00000020,name:"Dev team photo",code:"BLAMEUS",disabled:true,note:"Passcode screen only: typing BLAMEUS there shows the development team photo"}]},
    {title:"Luke skins",exclusive:true,items:[
      {bit:0x00000080,name:"R. Henke",code:"ICHHELD"},
      {bit:0x00000100,name:"M. Wagner",code:"PSYLOCK"},
      {bit:0x00000200,name:"J. Eggebrecht",code:"WUTZI"},
      {bit:0x00000400,name:"H. Schmidt",code:"BERGLOWE"},
      {bit:0x00000800,name:"F. Sauer",code:"TIECK"},
      {bit:0x00001000,name:"R. Stember",code:"RUDIBUBI"},
      {bit:0x00002000,name:"C. Huelsbeck",code:"CHIPPIE"},
      {bit:0x00004000,name:"T. Richter",code:"TOBIASS"},
      {bit:0x00008000,name:"J. Petersam",code:"SIRHISS"},
      {bit:0x00010000,name:"T. Engel",code:"HARDROCK"},
      {bit:0x00020000,name:"B. Hoppe",code:"THBPILOT"},
      {bit:0x00040000,name:"D. Jagalski",code:"FLYDODGE"}]}
  ];

  var buf=new Uint8Array(FILE_SIZE);
  var dv=new DataView(buf.buffer);
  var noSave=true;

  // ---- byte helpers (N64 big-endian) ----
  function u8(o){return buf[o];}
  function su8(o,v){buf[o]=v&0xFF;}
  function u16(o){return dv.getUint16(o,false);}
  function su16(o,v){dv.setUint16(o,v&0xFFFF,false);}
  function u32(o){return dv.getUint32(o,false)>>>0;}
  function su32(o,v){dv.setUint32(o,v>>>0,false);}

  // record/body offsets
  function bodyO(rec,o){return rec+BODY_OFF+o;}
  function acctO(rec,i,o){return rec+BODY_OFF+0x14+i*0x14+o;}
  function eliteO(rec,i,o){return rec+BODY_OFF+0x52+i*0x08+o;}

  // write a body byte to BOTH copies (structured edits stay mirrored)
  function setBody8(o,v){ for(var k=0;k<REC.length;k++) buf[REC[k]+BODY_OFF+o]=v&0xFF; }
  function setBody32(o,v){ for(var k=0;k<REC.length;k++) dv.setUint32(REC[k]+BODY_OFF+o,v>>>0,false); }
  function setAcct8(i,o,v){ for(var k=0;k<REC.length;k++) buf[REC[k]+BODY_OFF+0x14+i*0x14+o]=v&0xFF; }
  function setAcct32(i,o,v){ for(var k=0;k<REC.length;k++) dv.setUint32(REC[k]+BODY_OFF+0x14+i*0x14+o,v>>>0,false); }
  function setElite8(i,o,v){ for(var k=0;k<REC.length;k++) buf[REC[k]+BODY_OFF+0x52+i*0x08+o]=v&0xFF; }
  function setElite16(i,o,v){ for(var k=0;k<REC.length;k++) dv.setUint16(REC[k]+BODY_OFF+0x52+i*0x08+o,v&0xFFFF,false); }

  // ---- adler32 (zlib): init 1, mod 65521 ----
  function adler32(bytes,start,len){
    var a=1,b=0,i=start,end=start+len;
    while(i<end){
      var n=Math.min(end-i,3800);
      for(var j=0;j<n;j++){a+=bytes[i++];b+=a;}
      a%=65521;b%=65521;
    }
    return ((b<<16)|a)>>>0;
  }

  // ---- checksums + header ----
  function rebuildHeader(){
    su32(0x04,0xF5F5F5F5); su32(0x08,0x47535953); su32(0x0C,0xAAAAAAAA); // "GSYS"
    su32(0x00,adler32(buf,0x04,0x0C));
    su32(0x14,0x47414D45); su32(0x18,0x00B00002); su32(0x1C,0x55555555); // "GAME", (0xB0<<16)|2
    su32(0x10,adler32(buf,0x14,0x0C));
  }
  function recomputeAll(){
    for(var k=0;k<REC.length;k++){
      var rec=REC[k];
      su32(rec+0x04,adler32(buf,rec+UNK08_OFF,UNK08_SIZE));
      su32(rec+0x00,adler32(buf,rec+BODY_OFF,BODY_SIZE));
    }
    rebuildHeader();
  }
  function checksumsValid(){
    for(var k=0;k<REC.length;k++){
      var rec=REC[k];
      if(u32(rec+0x04)!==adler32(buf,rec+UNK08_OFF,UNK08_SIZE)) return false;
      if(u32(rec+0x00)!==adler32(buf,rec+BODY_OFF,BODY_SIZE)) return false;
    }
    if(u32(0x00)!==adler32(buf,0x04,0x0C)||u32(0x10)!==adler32(buf,0x14,0x0C)) return false;
    if(u32(0x04)!==0xF5F5F5F5||u32(0x08)!==0x47535953||u32(0x14)!==0x47414D45) return false;
    return true;
  }

  // ---- medals (byte = acct+0x06+(lvl>>2); 2 bits at (lvl&3)*2) ----
  function getMedal(i,lvl){ var b=u8(acctO(REC[0],i,0x06+(lvl>>2))); return (b>>((lvl&3)*2))&3; }
  function setMedal(i,lvl,m){
    var o=0x06+(lvl>>2), sh=(lvl&3)*2;
    for(var k=0;k<REC.length;k++){ var p=acctO(REC[k],i,o); buf[p]=(buf[p]&~(3<<sh))|((m&3)<<sh); }
  }

  // ---- name helpers ----
  function readName(base,len){ var s=""; for(var j=0;j<len;j++){ var c=buf[base+j]; if(c===0)break; s+=(c>=32&&c<127)?String.fromCharCode(c):"?"; } return s; }
  function writeName(setter,str,len){ str=(str||"").toUpperCase(); for(var j=0;j<len;j++){ var c=(j<str.length)?str.charCodeAt(j)&0xFF:0; setter(j,c);} }

  function acctActive(i){ return (u8(bodyO(REC[0],0x50))>>i)&1; }
  function setAcctActive(i,on){ var m=u8(bodyO(REC[0],0x50)); m=on?(m|(1<<i)):(m&~(1<<i)); setBody8(0x50,m); }

  // ---- pilot slots: add, delete, move (account block 0x14 bytes at body +0x14; active mask 0x50; pre-selected slot 0x51) ----
  var ACCT_SIZE=0x14;
  function clearAcct(i){ for(var k=0;k<REC.length;k++) buf.fill(0,acctO(REC[k],i,0),acctO(REC[k],i,ACCT_SIZE)); }
  // As createAccountProfile does: a cleared account, the first pilot ID (1-254) no Elite Rogues row uses, and every weapon
  // upgrade when the all-weapons cheat is on.
  function addPilot(i){
    clearAcct(i);
    var taken={}, id=1;
    for(var r=0;r<N_ELITE;r++) taken[u8(eliteO(REC[0],r,0x06))]=1;
    for(var a=0;a<N_ACCTS;a++) if(a!==i&&acctActive(a)) taken[u8(acctO(REC[0],a,0x10))]=1;
    while(id<255&&taken[id]) id++;
    setAcct8(i,0x10,id);
    if(u32(bodyO(REC[0],0x08))&0x01000000) setAcct32(i,0x0C,0x0001FE00);
    setAcctActive(i,true);
  }
  // The pre-selected slot moves to the first pilot left, so the front end never pre-selects an empty slot.
  function deletePilot(i){
    clearAcct(i); setAcctActive(i,false);
    if(u8(bodyO(REC[0],0x51))!==i) return;
    for(var j=0;j<N_ACCTS;j++) if(acctActive(j)){ setBody8(0x51,j); return; }
    setBody8(0x51,0);
  }
  // Moves slot from to position to; the slots between shift over, and each pilot keeps its active bit and pre-selection.
  function movePilot(from,to){
    var order=[]; for(var j=0;j<N_ACCTS;j++) order.push(j);
    order.splice(to,0,order.splice(from,1)[0]);
    var blocks=REC.map(function(rec){ return order.map(function(s){ return buf.slice(acctO(rec,s,0),acctO(rec,s,ACCT_SIZE)); }); });
    var mask=u8(bodyO(REC[0],0x50)), nm=mask&~((1<<N_ACCTS)-1), sel=u8(bodyO(REC[0],0x51));
    REC.forEach(function(rec,k){ order.forEach(function(s,pos){ buf.set(blocks[k][pos],acctO(rec,pos,0)); }); });
    order.forEach(function(s,pos){ if((mask>>s)&1) nm|=1<<pos; });
    setBody8(0x50,nm);
    if(order.indexOf(sel)>=0) setBody8(0x51,order.indexOf(sel));
  }
  var ICON={
    grip:'<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><g fill="currentColor"><circle cx="5.5" cy="3.5" r="1.3"/><circle cx="10.5" cy="3.5" r="1.3"/><circle cx="5.5" cy="8" r="1.3"/><circle cx="10.5" cy="8" r="1.3"/><circle cx="5.5" cy="12.5" r="1.3"/><circle cx="10.5" cy="12.5" r="1.3"/></g></svg>',
    trash:'<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" d="M2.5 4h11M6.5 4V2.5h3V4M4 4l.7 9.5h6.6L12 4M6.8 6.8v4.4M9.2 6.8v4.4"/></svg>',
    plus:'<svg viewBox="0 0 16 16" width="34" height="34" aria-hidden="true"><path fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" d="M8 2.5v11M2.5 8h11"/></svg>'
  };

  // ---- rendering ----
  function levelOpts(sel){ var s=""; for(var v=0;v<=16;v++) s+='<option value="'+v+'"'+(v===sel?' selected':'')+'>'+attr(levelLabel(v))+'</option>'; return s; }
  function medalTitle(l,m){ return levelLabel(l)+" ("+MEDAL_WORDS[m]+")"; }

  function renderPilots(){
    var host=document.getElementById("pilots"); host.innerHTML="";
    for(var i=0;i<N_ACCTS;i++){
      var active=acctActive(i);
      var name=readName(acctO(REC[0],i,0x00),4);
      var lvl=u8(acctO(REC[0],i,0x05));
      var card=document.createElement("div");
      card.className="card"+(active?"":" inactive");
      var med="";
      for(var l=0;l<N_LEVELS_MEDAL;l++){ var m=getMedal(i,l);
        med+='<button class="med" data-m="'+m+'" data-i="'+i+'" data-l="'+l+'" title="'+attr(medalTitle(l,m))+'"><b>'+(l+1)+'</b><span class="v">'+MEDAL_NAMES[m]+'</span></button>';
      }
      var uw=u32(acctO(REC[0],i,0x0C))>>>0, upg="";
      for(var u=0;u<UPGRADE_BITS.length;u++){ var ub=UPGRADE_BITS[u], on=(uw & ub.bit)>>>0!==0;
        upg+='<label class="upg" title="'+attr(ub.desc)+'">'+
          '<input type="checkbox" data-uacct="'+i+'" data-ubit="'+ub.bit+'"'+(on?' checked':'')+'><span>'+ub.name+'</span></label>';
      }
      card.setAttribute("data-slot",i);
      card.innerHTML=
        '<div class="toprow">'+(active?'<span class="grip" draggable="true" data-grip="'+i+'" title="Drag to another slot">'+ICON.grip+'</span>':'')+'<h3>Slot '+(i+1)+(active?' <span class="slot" title="Pilot ID: the Elite Rogues row with this ID is this pilot’s">ID '+u8(acctO(REC[0],i,0x10))+'</span>':'')+'</h3>'+
          (active?'<button class="iconbtn" data-trash="'+i+'" title="Delete this pilot" aria-label="Delete this pilot">'+ICON.trash+'</button>':'')+'</div>'+
        (active?'':'<button class="addpilot" data-add="'+i+'" title="Add a pilot in this slot">'+ICON.plus+'<span>Add pilot</span></button>')+
        '<div class="cardbody"'+(active?'':' inert')+'>'+
        '<div class="row2">'+
          '<div class="field"><label>Name</label><input class="mono" maxlength="3" data-name="'+i+'" value="'+name.replace(/"/g,'')+'" title="The pilot\'s initials, up to 3 letters, as entered on the ENTER NAME screen"></div>'+
          '<div class="field"><label>Current level</label><select data-lvl="'+i+'" title="How far this pilot has got: SELECT LEVEL lists every campaign level up to this one (The Battle of Calamari at most). It does not pick where the cursor starts; the bonus levels unlock separately">'+levelOpts(lvl)+'</select></div>'+
        '</div>'+
        '<label class="pick" title="The profile SELECT GAME highlights first when the game starts"><input type="radio" name="selacct" data-sel="'+i+'"'+(u8(bodyO(REC[0],0x51))===i?' checked':'')+'><span>Pre-select profile</span></label>'+
        '<div class="medals"><div class="lbl"><span>Level medals</span></div><div class="medgrid">'+med+'</div></div>'+
        '<div class="medals"><div class="lbl"><span>Weapon upgrades</span></div><div class="upggrid">'+upg+'</div></div></div>';
      host.appendChild(card);
    }
  }

  function score(b,s,g){ return (b&0x1F)+((s&0x1F)*2)+((g&0x1F)*4); }
  document.getElementById("rankHead").title="Rank by score (each 4 points is one rank):\n"+RANKS.map(function(r,k){ return (k*4)+(k===15?"+":"-"+(k*4+3))+": "+r; }).join("\n");
  function eliteScore(i){ var mb=u16(eliteO(REC[0],i,0x04)); return score(mb&0x1F,(mb>>5)&0x1F,(mb>>10)&0x1F); }
  // The game keeps the board in score order, highest first (highScoreBubbleSort swaps only a strictly lower score, so ties keep
  // their order); the editor re-sorts the same way after a medal count changes.
  function sortElite(){
    var order=[]; for(var i=0;i<N_ELITE;i++) order.push(i);
    order.sort(function(a,b){ return eliteScore(b)-eliteScore(a)||a-b; });
    REC.forEach(function(rec){
      var rows=order.map(function(s){ return buf.slice(eliteO(rec,s,0),eliteO(rec,s,8)); });
      rows.forEach(function(r,pos){ buf.set(r,eliteO(rec,pos,0)); });
    });
  }
  function renderElite(){
    var tb=document.getElementById("eliteBody"); tb.innerHTML="";
    for(var i=0;i<N_ELITE;i++){
      var name=readName(eliteO(REC[0],i,0x00),3);
      var lvl=u8(eliteO(REC[0],i,0x03));
      var mb=u16(eliteO(REC[0],i,0x04));
      var br=mb&0x1F, sv=(mb>>5)&0x1F, gd=(mb>>10)&0x1F;
      var acct=u8(eliteO(REC[0],i,0x06));
      var sc=score(br,sv,gd), rk=RANKS[Math.min(sc>>2,15)];
      var tr=document.createElement("tr");
      tr.innerHTML='<td class="num">'+(i+1)+'</td>'+
        '<td><input class="mini mono" maxlength="3" data-ename="'+i+'" value="'+name.replace(/"/g,'')+'" title="Initials shown for this place on the Elite Rogues board"></td>'+
        '<td class="num"><input class="mini" type="number" min="0" max="18" data-elvl="'+i+'" value="'+lvl+'" title="'+attr("The level shown for this entry, 0-18 ("+levelLabel(clamp(lvl,0,18))+")")+'"></td>'+
        '<td class="num"><input class="mini" type="number" min="0" max="31" data-ebr="'+i+'" value="'+br+'" title="Bronze medals, 0-31; each adds 1 to the score"></td>'+
        '<td class="num"><input class="mini" type="number" min="0" max="31" data-esv="'+i+'" value="'+sv+'" title="Silver medals, 0-31; each adds 2 to the score"></td>'+
        '<td class="num"><input class="mini" type="number" min="0" max="31" data-egd="'+i+'" value="'+gd+'" title="Gold medals, 0-31; each adds 4 to the score"></td>'+
        '<td class="num"><input class="mini" type="number" min="0" max="255" data-eacct="'+i+'" value="'+acct+'" title="The pilot this row belongs to (the ID on their pilot card); 0 = one of the game’s default entries"></td>'+
        '<td class="num" data-score="'+i+'">'+sc+'</td>'+
        '<td class="rank" data-rank="'+i+'">'+rk+'</td>';
      tb.appendChild(tr);
    }
  }

  function renderSettings(){
    ["volMusic:0x10","volSfx:0x11","volSpeech:0x12"].forEach(function(p){
      var id=p.split(":")[0], v=u8(bodyO(REC[0],parseInt(p.split(":")[1],16)));
      document.getElementById(id).value=v;
      document.getElementById(id+"Val").textContent=Math.min(v,127);
    });
    setSelect("ctrlSet",u8(bodyO(REC[0],0x13)));
    setSelect("langSel",u8(bodyO(REC[0],0xA8)));
  }
  // A value the list does not name still shows, as "Unknown (n)", rather than the select silently picking another.
  function setSelect(id,v){
    var s=document.getElementById(id);
    [].slice.call(s.querySelectorAll("option.unknown")).forEach(function(o){ o.remove(); });
    if(![].some.call(s.options,function(o){ return +o.value===v; })) s.append(Object.assign(new Option("Unknown ("+v+")",String(v)),{className:"unknown"}));
    s.value=String(v);
  }

  function renderStatus(){
    var fp=document.getElementById("filePill"), ok=checksumsValid();
    document.getElementById("fileName").textContent=noSave?"No save loaded":(currentName||"loaded save");
    fp.className="pill "+(noSave?"warn":"ok");
    document.body.classList.toggle("nosave",noSave);
    // Edits keep the checksums right; this shows whether a hand poke on the Raw tab still needs a recompute.
    var cp=document.getElementById("chkPill");
    cp.hidden=noSave;
    cp.className="pill "+(ok?"ok":"bad");
    document.getElementById("chkText").textContent=ok?"checksums OK":"checksums INVALID";
    cp.title=ok?"Both copies of the save pass the game's checksum check":"The checksums do not match, so the game would reject this save: click to recompute them";
    cp.style.cursor=ok?"":"pointer";
    var ex=document.getElementById("exportBtn"), es=document.getElementById("extSel");
    ex.disabled=es.disabled=noSave;
    ex.title=noSave?"Load a save first":"Download the edited save, named like the loaded file with the chosen extension";
  }

  function renderUnlocks(){
    var w=u32(bodyO(REC[0],0x08))>>>0, host=document.getElementById("unlocks"), html="";
    for(var g=0;g<CHEAT_GROUPS.length;g++){ var grp=CHEAT_GROUPS[g];
      html+='<div class="grp"><h4>'+grp.title+'</h4>';
      if(grp.exclusive){
        var allMask=0; for(var m=0;m<grp.items.length;m++) allMask|=grp.items[m].bit;
        var opts='<option value="0">None</option>';
        for(var s=0;s<grp.items.length;s++){ var gi=grp.items[s], sel=(w & gi.bit)>>>0!==0;
          opts+='<option value="'+gi.bit+'"'+(sel?' selected':'')+'>'+gi.name+' ('+gi.code+')</option>'; }
        html+='<select class="skinsel" data-mask="'+(allMask>>>0)+'" title="Which developer face replaces Luke, as its cheat code would set it; only one at a time">'+opts+'</select>';
      } else {
        html+='<div class="chkgrid">';
        for(var k=0;k<grp.items.length;k++){ var it=grp.items[k], on=it.naboo?u32(bodyO(REC[0],0xAC))===NABOO_KEY:(w & it.bit)>>>0!==0;
          var tt=' title="'+attr(it.note||('Same as typing '+it.code+' on the passcode screen: '+it.name+' stays unlocked for every pilot'))+'"';
          html+='<label class="chk'+(it.disabled?' chk-disabled':'')+'"'+tt+'><input type="checkbox" '+(it.naboo?'data-naboo="1"':'data-bit="'+it.bit+'"')+(on?' checked':'')+(it.disabled?' disabled':'')+'><span>'+it.name+'</span><span class="code">'+it.code+'</span></label>';
        }
        html+='</div>';
      }
      html+='</div>';
    }
    host.innerHTML=html;
  }
  function renderAll(){ renderPilots(); renderElite(); renderSettings(); renderUnlocks(); renderStatus(); }

  // ---- event wiring (delegated) ----
  document.getElementById("pilots").addEventListener("click",function(e){
    var add=e.target.closest("[data-add]"), del=e.target.closest("[data-trash]");
    if(add){
      var ai0=+add.getAttribute("data-add");
      addPilot(ai0); recomputeAll(); renderAll();
      document.querySelector('[data-name="'+ai0+'"]').focus();
      return;
    }
    if(del){
      var di=+del.getAttribute("data-trash"), nm=readName(acctO(REC[0],di,0x00),4).trim();
      if(!confirm("Delete "+(nm?"pilot "+nm:"the pilot")+" in slot "+(di+1)+"? Its name, medals and upgrades are cleared. The file on disk changes only when you export."))return;
      deletePilot(di); recomputeAll(); renderAll();
      return;
    }
    var t=e.target.closest("[data-l]"); if(!t)return;
    var ai=+t.getAttribute("data-i"), lv=+t.getAttribute("data-l"), m=(getMedal(ai,lv)+1)&3;
    setMedal(ai,lv,m); recomputeAll();
    t.setAttribute("data-m",m); t.title=medalTitle(lv,m); t.querySelector(".v").textContent=MEDAL_NAMES[m]; renderStatus();
  });
  document.getElementById("pilots").addEventListener("change",function(e){
    var t=e.target, i;
    if(t.hasAttribute("data-sel")){ i=+t.getAttribute("data-sel"); setBody8(0x51,i); }
    else if(t.hasAttribute("data-lvl")){ i=+t.getAttribute("data-lvl"); setAcct8(i,0x05,+t.value); }
    else if(t.hasAttribute("data-name")){ i=+t.getAttribute("data-name"); (function(idx){writeName(function(j,c){setAcct8(idx,0x00+j,c);},t.value,4);})(i); }
    else if(t.hasAttribute("data-ubit")){ i=+t.getAttribute("data-uacct"); var ub=(+t.getAttribute("data-ubit"))>>>0, uw=u32(acctO(REC[0],i,0x0C))>>>0; setAcct32(i,0x0C,(t.checked?(uw|ub):(uw&~ub))>>>0); recomputeAll(); renderStatus(); return; }
    else return;
    recomputeAll(); renderAll();
  });

  // Dragging a pilot's grip onto another slot moves it there.
  var dragFrom=-1, pilotsEl=document.getElementById("pilots");
  pilotsEl.addEventListener("dragstart",function(e){
    var g=e.target.closest&&e.target.closest("[data-grip]"); if(!g)return;
    var card=g.closest(".card");
    dragFrom=+g.getAttribute("data-grip");
    e.dataTransfer.effectAllowed="move";
    e.dataTransfer.setData("text/plain",String(dragFrom));
    e.dataTransfer.setDragImage(card,24,24);
    card.classList.add("dragging");
  });
  pilotsEl.addEventListener("dragover",function(e){
    var c=e.target.closest(".card"); if(dragFrom<0||!c)return;
    e.preventDefault();
    pilotsEl.querySelectorAll(".dropover").forEach(function(x){ if(x!==c) x.classList.remove("dropover"); });
    if(+c.getAttribute("data-slot")!==dragFrom) c.classList.add("dropover");
  });
  pilotsEl.addEventListener("drop",function(e){
    var c=e.target.closest(".card"); if(dragFrom<0||!c)return;
    e.preventDefault();
    var to=+c.getAttribute("data-slot"), from=dragFrom;
    dragFrom=-1;
    if(to!==from){ movePilot(from,to); recomputeAll(); }
    renderAll();
  });
  pilotsEl.addEventListener("dragend",function(){
    dragFrom=-1;
    pilotsEl.querySelectorAll(".dropover,.dragging").forEach(function(x){ x.classList.remove("dropover","dragging"); });
  });

  // Number fields never show a value their byte cannot hold: typing past min/max snaps to the limit.
  document.addEventListener("keydown",function(e){
    if(e.target.type==="number"&&/^[eE+\-.,]$/.test(e.key)) e.preventDefault();
  });
  // Runs before the fields' own change handlers: an emptied number field takes its minimum.
  document.addEventListener("change",function(e){
    var t=e.target; if(t.type==="number"&&t.value==="") t.value=t.min||0;
  },true);
  document.addEventListener("input",function(e){
    var t=e.target;
    // Initials: capital letters only, as the ENTER NAME screen offers.
    if(t.hasAttribute("data-name")||t.hasAttribute("data-ename")){
      var n=t.value.toUpperCase().replace(/[^A-Z]/g,"").slice(0,3);
      if(n!==t.value) t.value=n;
      return;
    }
    if(t.type!=="number"||t.value==="")return;
    var v=Math.trunc(+t.value), lo=t.min===""?-Infinity:+t.min, hi=t.max===""?Infinity:+t.max;
    if(isNaN(v)) return;
    var c=Math.min(hi,Math.max(lo,v));
    if(String(c)!==t.value) t.value=c;
  });
  document.getElementById("eliteBody").addEventListener("change",function(e){
    var t=e.target,i;
    if(t.hasAttribute("data-ename")){ i=+t.getAttribute("data-ename"); (function(idx){writeName(function(j,c){setElite8(idx,0x00+j,c);},t.value,3);})(i); }
    else if(t.hasAttribute("data-elvl")){ i=+t.getAttribute("data-elvl"); setElite8(i,0x03,clamp(+t.value,0,18)); t.title="The level shown for this entry, 0-18 ("+levelLabel(clamp(+t.value,0,18))+")"; }
    else if(t.hasAttribute("data-eacct")){ i=+t.getAttribute("data-eacct"); setElite8(i,0x06,clamp(+t.value,0,255)); }
    else if(t.hasAttribute("data-ebr")||t.hasAttribute("data-esv")||t.hasAttribute("data-egd")){
      i=+(t.getAttribute("data-ebr")||t.getAttribute("data-esv")||t.getAttribute("data-egd"));
      var row=t.closest("tr");
      var br=clamp(+row.querySelector("[data-ebr]").value,0,31);
      var sv=clamp(+row.querySelector("[data-esv]").value,0,31);
      var gd=clamp(+row.querySelector("[data-egd]").value,0,31);
      setElite16(i,0x04,(br&0x1F)|((sv&0x1F)<<5)|((gd&0x1F)<<10));
      sortElite(); recomputeAll(); renderElite(); renderStatus();
      return;
    } else return;
    recomputeAll(); renderStatus();
  });

  ["volMusic:0x10","volSfx:0x11","volSpeech:0x12","ctrlSet:0x13","langSel:0xA8"].forEach(function(pair){
    var id=pair.split(":")[0], off=parseInt(pair.split(":")[1],16);
    var el=document.getElementById(id), shown=document.getElementById(id+"Val");
    if(shown) el.addEventListener("input",function(){ shown.textContent=el.value; });
    el.addEventListener("change",function(){ setBody8(off,clamp(+this.value,0,255)); recomputeAll(); renderStatus(); });
  });

  document.getElementById("unlocks").addEventListener("change",function(e){
    var t=e.target, w=u32(bodyO(REC[0],0x08))>>>0;
    if(t.hasAttribute("data-bit")){
      var bit=(+t.getAttribute("data-bit"))>>>0;
      w=t.checked?(w|bit)>>>0:(w&~bit)>>>0;
    } else if(t.hasAttribute("data-naboo")){
      // As applyCheatCodeFromInput does for !YNGWIE!: the HALIFAX? flag is spent and the checked key saved.
      setBody32(0xAC,t.checked?NABOO_KEY:0);
      w=(w&~0x10)>>>0;
    } else if(t.hasAttribute("data-mask")){
      var mask=(+t.getAttribute("data-mask"))>>>0, val=(+t.value)>>>0; // exclusive: clear group, set one
      w=(((w&~mask)>>>0)|val)>>>0;
    } else return;
    setBody32(0x08,w); recomputeAll();
    renderStatus();
  });

  // Edits keep the checksums right, so the pill only turns red if something did not; clicking it then recomputes them.
  document.getElementById("chkPill").addEventListener("click",function(){
    if(noSave||checksumsValid())return;
    recomputeAll(); renderAll(); toast("Checksums recomputed.");
  });

  // ---- utilities ----
  function clamp(v,lo,hi){ v=v|0; return v<lo?lo:(v>hi?hi:v); }

  // ---- load / new / export ----
  var currentName="";
  // Only a real save is taken: at least 512 bytes whose header markers and checksums already match the game's. Anything else is
  // refused and the current state kept, so the editor can never be used to make a save from nothing.
  function loadBytes(bytes,name){
    var keep=[buf,dv];
    if(bytes.length<FILE_SIZE){ toast("Not a Rogue Squadron save: it is "+bytes.length+" bytes, a save is "+FILE_SIZE+". Nothing was loaded.",true); return; }
    buf=bytes.slice(0,FILE_SIZE); dv=new DataView(buf.buffer);
    if(!checksumsValid()){
      buf=keep[0]; dv=keep[1];
      toast("Not a Rogue Squadron save: its header or checksums do not match the game's. Nothing was loaded.",true);
      return;
    }
    currentName=name||""; noSave=false;
    var m=/(\.[^.]+)$/.exec(currentName); // match dropdown to the loaded extension
    if(m){ var e=m[1].toLowerCase(), sel=document.getElementById("extSel");
      for(var o=0;o<sel.options.length;o++){ if(sel.options[o].value===e){ sel.value=e; break; } } }
    renderAll();
  }
  document.getElementById("loadBtn").addEventListener("click",function(){ document.getElementById("fileInput").click(); });
  function loadFile(f){
    var fr=new FileReader();
    fr.onload=function(){ loadBytes(new Uint8Array(fr.result),f.name); };
    fr.readAsArrayBuffer(f);
  }
  document.getElementById("fileInput").addEventListener("change",function(e){
    var f=e.target.files&&e.target.files[0]; if(f) loadFile(f);
    e.target.value="";
  });
  // Until a save is open, the drop box opens the file dialog and a file dropped anywhere on the page loads.
  var dropBox=document.getElementById("dropBox");
  dropBox.addEventListener("click",function(){ document.getElementById("fileInput").click(); });
  dropBox.addEventListener("keydown",function(e){ if(e.key==="Enter"||e.key===" "){ e.preventDefault(); document.getElementById("fileInput").click(); } });
  addEventListener("dragover",function(e){ if(!noSave||!e.dataTransfer.types.includes("Files"))return; e.preventDefault(); document.body.classList.add("dragover"); });
  addEventListener("dragleave",function(e){ if(!e.relatedTarget) document.body.classList.remove("dragover"); });
  addEventListener("drop",function(e){
    document.body.classList.remove("dragover");
    if(!noSave||!e.dataTransfer.files[0])return;
    e.preventDefault();
    loadFile(e.dataTransfer.files[0]);
  });

  // CRC32 + a minimal store-only ZIP writer (for the sandbox, where the
  // downloads capability only accepts a fixed extension allowlist that has no
  // raw-binary type; .eep goes inside a .zip).
  var _crcT=(function(){var t=[];for(var n=0;n<256;n++){var c=n;for(var k=0;k<8;k++)c=(c&1)?(0xEDB88320^(c>>>1)):(c>>>1);t[n]=c>>>0;}return t;})();
  function crc32(b){var c=0xFFFFFFFF;for(var i=0;i<b.length;i++)c=_crcT[(c^b[i])&0xFF]^(c>>>8);return (c^0xFFFFFFFF)>>>0;}
  function zipStore(name,data){
    var nb=new TextEncoder().encode(name), nlen=nb.length, dlen=data.length, crc=crc32(data);
    var out=new Uint8Array(30+nlen+dlen+46+nlen+22), dvz=new DataView(out.buffer), p=0;
    function w32(v){dvz.setUint32(p,v>>>0,true);p+=4;} function w16(v){dvz.setUint16(p,v&0xFFFF,true);p+=2;} function wb(x){out.set(x,p);p+=x.length;}
    w32(0x04034b50);w16(20);w16(0);w16(0);w16(0);w16(0);w32(crc);w32(dlen);w32(dlen);w16(nlen);w16(0);wb(nb);wb(data);
    var cdOff=p;
    w32(0x02014b50);w16(20);w16(20);w16(0);w16(0);w16(0);w16(0);w32(crc);w32(dlen);w32(dlen);w16(nlen);w16(0);w16(0);w16(0);w16(0);w32(0);w32(0);wb(nb);
    var cdSize=p-cdOff;
    w32(0x06054b50);w16(0);w16(0);w16(1);w16(1);w32(cdSize);w32(cdOff);w16(0);
    return out;
  }

  function toast(msg,bad){
    var el=document.getElementById("toast"); el.textContent=msg; el.className="toast show"+(bad?" bad":"");
    clearTimeout(toast._t); toast._t=setTimeout(function(){ el.className="toast"; },4200);
  }

  async function exportSave(){
    // Only edits of a loaded save are exported, never a save built from scratch.
    if(noSave){ toast("Load your save file first.",true); return; }
    recomputeAll(); renderStatus();
    // Base name from the loaded file (or a default); extension from the dropdown.
    var base=(currentName&&!noSave)?currentName.replace(/\.[^.]*$/,""):"rogue_squadron";
    var ext=document.getElementById("extSel").value||".bin";
    var outName=base+ext;
    var raw=buf.slice();
    var dl=null;
    try{ dl = window.claude && window.claude.use ? await window.claude.use("downloads") : null; }catch(e){ dl=null; }
    if(dl){
      // Sandbox: raw save bytes aren't an allowed download type, so deliver a .zip of it.
      try{
        var zip=zipStore(outName,raw);
        var r=await dl.save({filename:outName+".zip",data:zip});
        if(r&&r.status==="saved") toast("Saved "+outName+".zip — unzip to get "+outName);
        return;
      }catch(err){
        if(err&&err.code==="declined"){ toast("Save cancelled."); return; }
        // otherwise fall through
      }
    }
    // Local file / normal browser: direct download, original name.
    try{
      var blob=new Blob([raw],{type:"application/octet-stream"});
      var a=document.createElement("a"); a.href=URL.createObjectURL(blob); a.download=outName;
      document.body.appendChild(a); a.click(); setTimeout(function(){URL.revokeObjectURL(a.href);a.remove();},0);
      toast("Downloaded "+outName);
      return;
    }catch(err2){}
    // Last resort: reveal base64 to copy out.
    var ta=document.getElementById("b64out"); ta.hidden=false;
    var s=""; for(var i=0;i<raw.length;i++) s+=String.fromCharCode(raw[i]);
    ta.value=btoa(s); ta.focus(); ta.select();
    toast("Download blocked here — copy the base64 from the Raw tab.",true);
  }
  document.getElementById("exportBtn").addEventListener("click",function(){ exportSave(); });

  // ---- tabs ----
  document.querySelectorAll(".tab").forEach(function(tab){
    tab.addEventListener("click",function(){
      document.querySelectorAll(".tab").forEach(function(t){t.setAttribute("aria-selected",t===tab?"true":"false");});
      ["pilots","elite","settings","unlocks"].forEach(function(n){ document.getElementById("tab-"+n).hidden=(n!==tab.getAttribute("data-tab")); });
    });
  });

  // ---- theme toggle (per-viewer convenience) ----
  document.getElementById("themeBtn").addEventListener("click",function(){
    var cur=document.documentElement.getAttribute("data-theme");
    var next=cur==="dark"?"light":(cur==="light"?"dark":(matchMedia("(prefers-color-scheme: dark)").matches?"light":"dark"));
    document.documentElement.setAttribute("data-theme",next);
    try{localStorage.setItem("rs64save.theme",next);}catch(e){}
  });
  try{var th=localStorage.getItem("rs64save.theme"); if(th)document.documentElement.setAttribute("data-theme",th);}catch(e){}

  // ---- WebMCP: tools an AI agent in the browser can call (registerTool, where the browser has it). Each answers JSON text and edits
  // through the same helpers as the page, so both copies and the checksums stay right; nothing is downloaded (export stays a click). ----
  function needSave(){ if(noSave) throw new Error("No save loaded: load one in the page, or pass its bytes to rs64save_load"); }
  function upgradeNames(w){ return UPGRADE_BITS.filter(function(u){ return (w&u.bit)>>>0!==0; }).map(function(u){ return u.name; }); }
  function cheatItems(){ return CHEAT_GROUPS.reduce(function(a,g){ return a.concat(g.items.map(function(it){ return {group:g.title,exclusive:!!g.exclusive,item:it}; })); },[]); }
  function readPilot(i){
    var active=!!acctActive(i);
    return {slot:i,active:active,preselected:u8(bodyO(REC[0],0x51))===i,pilotId:active?u8(acctO(REC[0],i,0x10)):null,name:readName(acctO(REC[0],i,0x00),4),
      level:u8(acctO(REC[0],i,0x05)),levelName:LEVEL_NAMES[u8(acctO(REC[0],i,0x05))]||null,
      medals:LEVEL_NAMES.map(function(n,l){ return {level:l,name:n,medal:MEDAL_WORDS[getMedal(i,l)]}; }),upgrades:upgradeNames(u32(acctO(REC[0],i,0x0C)))};
  }
  function readElite(i){
    var mb=u16(eliteO(REC[0],i,0x04)), sc=eliteScore(i);
    return {row:i,name:readName(eliteO(REC[0],i,0x00),3),level:u8(eliteO(REC[0],i,0x03)),bronze:mb&0x1F,silver:(mb>>5)&0x1F,gold:(mb>>10)&0x1F,pilotId:u8(eliteO(REC[0],i,0x06)),score:sc,rank:RANKS[Math.min(sc>>2,15)]};
  }
  function readUnlocks(){
    var w=u32(bodyO(REC[0],0x08))>>>0, out={};
    cheatItems().forEach(function(c){ if(!c.exclusive) out[c.item.name]=c.item.naboo?u32(bodyO(REC[0],0xAC))===NABOO_KEY:(w&c.item.bit)>>>0!==0; });
    var skin=cheatItems().filter(function(c){ return c.exclusive&&(w&c.item.bit)>>>0!==0; })[0];
    out["Luke skin"]=skin?skin.item.name:"None";
    return out;
  }
  function readAll(){
    var p=[]; for(var i=0;i<N_ACCTS;i++) p.push(readPilot(i));
    var e=[]; for(var r=0;r<N_ELITE;r++) e.push(readElite(r));
    return {file:currentName||null,checksumsOk:checksumsValid(),pilots:p,eliteRogues:e,
      settings:{music:u8(bodyO(REC[0],0x10)),sfx:u8(bodyO(REC[0],0x11)),speech:u8(bodyO(REC[0],0x12)),controllerLayout:u8(bodyO(REC[0],0x13)),language:u8(bodyO(REC[0],0xA8))},unlocks:readUnlocks()};
  }
  function slotOf(s){ if(!(s>=0&&s<N_ACCTS)) throw new Error("slot is 0, 1 or 2"); return s; }
  function initials(s){ s=String(s).toUpperCase(); if(!/^[A-Z]{0,3}$/.test(s)) throw new Error("Initials are up to 3 letters A-Z, as the ENTER NAME screen offers"); return s; }
  function edited(r){ recomputeAll(); renderAll(); return r; }
  var MCP_TOOLS=[
    {name:"rs64save_status",description:"Whether a save is loaded in the RS64 Save Editor, its file name and whether its checksums pass.",inputSchema:{type:"object",properties:{}},
      run:function(){ return {loaded:!noSave,file:noSave?null:(currentName||null),checksumsOk:noSave?null:checksumsValid()}; }},
    {name:"rs64save_load",edit:true,description:"Loads a save from its bytes (base64; 512 bytes, the recomp's saves/<game-id>.bin or a cart EEPROM dump). Anything that is not a real Rogue Squadron save (wrong size, header or checksums) is refused and the open save kept.",
      inputSchema:{type:"object",properties:{base64:{type:"string"},name:{type:"string"}},required:["base64"]},
      run:function(a){
        var s=atob(a.base64), b=new Uint8Array(s.length); for(var i=0;i<s.length;i++) b[i]=s.charCodeAt(i);
        var was=buf; loadBytes(b,a.name||"agent.bin");
        if(buf===was) throw new Error("Not a Rogue Squadron save: its size, header or checksums do not match the game's");
        return readAll();
      }},
    {name:"rs64save_read",description:"Everything in the loaded save, decoded: the three pilots (name, current level, a medal per level, weapon upgrades, pilot ID, pre-selected), the Elite Rogues board (name, level, medal counts, pilot ID, score, rank), settings (volumes 0-127, controller layout 0-3, language 0 English 1 Deutsch 2 Francais) and the cheat unlocks.",
      inputSchema:{type:"object",properties:{}},run:function(){ needSave(); return readAll(); }},
    {name:"rs64save_bytes",description:"The edited save's bytes as base64 (checksums recomputed), to hand to someone or check offline. Saving it as a file stays the page's Export button.",
      inputSchema:{type:"object",properties:{}},run:function(){ needSave(); recomputeAll(); renderStatus(); var s=""; for(var i=0;i<buf.length;i++) s+=String.fromCharCode(buf[i]); return {name:currentName||null,base64:btoa(s)}; }},
    {name:"rs64save_set_pilot",edit:true,description:"Changes a pilot (slot 0-2, which must hold one: add it with rs64save_pilot_slot). name: initials; level: the current level 0-16 (SELECT LEVEL lists every campaign level up to it); medals: { level number 0-18: \"none\" | \"bronze\" | \"silver\" | \"gold\" }; upgrades: { upgrade name: true | false } (names as rs64save_read lists them, or every name with all: true / false); preselect: true makes it the profile SELECT GAME highlights.",
      inputSchema:{type:"object",properties:{slot:{type:"integer"},name:{type:"string"},level:{type:"integer",minimum:0,maximum:16},medals:{type:"object"},upgrades:{type:"object"},all:{type:"boolean"},preselect:{type:"boolean"}},required:["slot"]},
      run:function(a){
        needSave(); var i=slotOf(a.slot);
        if(!acctActive(i)) throw new Error("Slot "+i+" is empty: add a pilot there first");
        if(a.name!==undefined){ var n=initials(a.name); writeName(function(j,c){ setAcct8(i,j,c); },n,4); }
        if(a.level!==undefined) setAcct8(i,0x05,clamp(a.level,0,16));
        Object.keys(a.medals||{}).forEach(function(l){ var m=MEDAL_WORDS.indexOf(a.medals[l]); if(m<0) m=MEDAL_NAMES.indexOf(a.medals[l]);
          if(m<0||!(+l>=0&&+l<N_LEVELS_MEDAL)) throw new Error("medals: level 0-18 to none, bronze, silver or gold"); setMedal(i,+l,m); });
        var uw=u32(acctO(REC[0],i,0x0C))>>>0;
        if(a.all!==undefined) UPGRADE_BITS.forEach(function(u){ uw=a.all?(uw|u.bit)>>>0:(uw&~u.bit)>>>0; });
        Object.keys(a.upgrades||{}).forEach(function(k){ var u=UPGRADE_BITS.filter(function(x){ return x.name===k; })[0]; if(!u) throw new Error("Unknown upgrade "+k+": "+UPGRADE_BITS.map(function(x){ return x.name; }).join(", "));
          uw=a.upgrades[k]?(uw|u.bit)>>>0:(uw&~u.bit)>>>0; });
        setAcct32(i,0x0C,uw);
        if(a.preselect) setBody8(0x51,i);
        return edited(readPilot(i));
      }},
    {name:"rs64save_pilot_slot",edit:true,description:"Adds a pilot in an empty slot (a fresh profile with the next free pilot ID, as the game makes one), deletes one (its name, medals and upgrades cleared; the pre-selection moves to a pilot left), or moves one to another slot (the slots between shift over).",
      inputSchema:{type:"object",properties:{action:{type:"string",enum:["add","delete","move"]},slot:{type:"integer"},to:{type:"integer"}},required:["action","slot"]},
      run:function(a){
        needSave(); var i=slotOf(a.slot);
        if(a.action==="add"){ if(acctActive(i)) throw new Error("Slot "+i+" already holds a pilot"); addPilot(i); }
        else if(a.action==="delete"){ if(!acctActive(i)) throw new Error("Slot "+i+" is empty"); deletePilot(i); }
        else { var to=slotOf(a.to); if(to!==i) movePilot(i,to); }
        edited();
        var p=[]; for(var k=0;k<N_ACCTS;k++) p.push(readPilot(k));
        return {pilots:p.map(function(x){ return {slot:x.slot,active:x.active,name:x.name,pilotId:x.pilotId,preselected:x.preselected}; })};
      }},
    {name:"rs64save_set_elite",edit:true,description:"Changes a row (0-9) of the Elite Rogues board: name (initials), level (0-18), bronze / silver / gold (0-31 each; the score is gold x 4 + silver x 2 + bronze), pilotId (0-255; 0 = one of the game's default entries). The board re-sorts by score as the game keeps it.",
      inputSchema:{type:"object",properties:{row:{type:"integer",minimum:0,maximum:9},name:{type:"string"},level:{type:"integer"},bronze:{type:"integer"},silver:{type:"integer"},gold:{type:"integer"},pilotId:{type:"integer"}},required:["row"]},
      run:function(a){
        needSave(); var i=a.row; if(!(i>=0&&i<N_ELITE)) throw new Error("row is 0-9");
        if(a.name!==undefined){ var n=initials(a.name); writeName(function(j,c){ setElite8(i,j,c); },n,3); }
        if(a.level!==undefined) setElite8(i,0x03,clamp(a.level,0,18));
        if(a.pilotId!==undefined) setElite8(i,0x06,clamp(a.pilotId,0,255));
        var e=readElite(i), br=a.bronze!==undefined?clamp(a.bronze,0,31):e.bronze, sv=a.silver!==undefined?clamp(a.silver,0,31):e.silver, gd=a.gold!==undefined?clamp(a.gold,0,31):e.gold;
        setElite16(i,0x04,br|(sv<<5)|(gd<<10));
        sortElite(); edited();
        var rows=[]; for(var r=0;r<N_ELITE;r++) rows.push(readElite(r));
        return {eliteRogues:rows};
      }},
    {name:"rs64save_set_settings",edit:true,description:"Changes the save's options: music, sfx, speech volume (0-127), controllerLayout (0-3, the four layouts of the CONTROLLER SETTINGS screen), language (0 English, 1 Deutsch, 2 Francais).",
      inputSchema:{type:"object",properties:{music:{type:"integer"},sfx:{type:"integer"},speech:{type:"integer"},controllerLayout:{type:"integer",minimum:0,maximum:3},language:{type:"integer",minimum:0,maximum:2}}},
      run:function(a){
        needSave();
        [["music",0x10,127],["sfx",0x11,127],["speech",0x12,127],["controllerLayout",0x13,3],["language",0xA8,2]].forEach(function(f){ if(a[f[0]]!==undefined) setBody8(f[1],clamp(a[f[0]],0,f[2])); });
        return edited(readAll().settings);
      }},
    {name:"rs64save_set_unlocks",edit:true,description:"Sets the cheat unlocks shared by every pilot, as typing their codes on the passcode screen would: set maps an unlock name or its code (as rs64save_read lists them, e.g. \"Millennium Falcon\" or \"FARMBOY\") to true / false; skin picks the Luke skin by name or code (or \"None\"). Codes that only work when typed (AT-ST, credits, dev photo) are refused.",
      inputSchema:{type:"object",properties:{set:{type:"object"},skin:{type:"string"}}},
      run:function(a){
        needSave(); var w=u32(bodyO(REC[0],0x08))>>>0, items=cheatItems();
        Object.keys(a.set||{}).forEach(function(k){
          var c=items.filter(function(x){ return !x.exclusive&&(x.item.name===k||x.item.code===k); })[0];
          if(!c) throw new Error("Unknown unlock "+k);
          if(c.item.disabled) throw new Error(c.item.name+": "+c.item.note);
          if(c.item.naboo){ setBody32(0xAC,a.set[k]?NABOO_KEY:0); w=(w&~0x10)>>>0; }
          else w=a.set[k]?(w|c.item.bit)>>>0:(w&~c.item.bit)>>>0;
        });
        if(a.skin!==undefined){
          var skins=items.filter(function(x){ return x.exclusive; }), mask=skins.reduce(function(m,x){ return m|x.item.bit; },0)>>>0;
          var pick=a.skin==="None"?null:skins.filter(function(x){ return x.item.name===a.skin||x.item.code===a.skin; })[0];
          if(a.skin!=="None"&&!pick) throw new Error("Unknown skin "+a.skin+": "+skins.map(function(x){ return x.item.name; }).join(", "));
          w=(((w&~mask)>>>0)|(pick?pick.item.bit:0))>>>0;
        }
        setBody32(0x08,w);
        return edited(readUnlocks());
      }}
  ];
  var mcp=document.modelContext||navigator.modelContext;
  if(mcp&&mcp.registerTool) MCP_TOOLS.forEach(function(t){
    var tool={name:t.name,description:t.description,inputSchema:t.inputSchema,annotations:{readOnlyHint:!t.edit},
      // A thrown error reaches the agent only as "the invocation failed", so its message is returned instead.
      execute:async function(input){
        try{ return {content:[{type:"text",text:JSON.stringify(await t.run(input||{}))}]}; }
        catch(e){ return {content:[{type:"text",text:"Error: "+e.message}],isError:true}; }
      }};
    Promise.resolve().then(function(){ return mcp.registerTool(tool); }).catch(function(e){ console.warn("WebMCP: "+t.name+" not registered:",e); });
  });

  // ---- boot: nothing to edit until a real save is loaded (the editor never builds one itself) ----
  renderStatus();
})();
