function doGet(e) {
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    var params = e.parameter;

    sheet.appendRow([
      new Date(),
      params.mac   || "",
      parseFloat(params.v)     || 0,
      parseInt(params.drift)   || 0,
      parseInt(params.rssi)    || 0,
      parseFloat(params.temp)  || 0,
      params.fw    || "",
      parseInt(params.days)    || 0,
      parseFloat(params.lat)   || 0,
      parseFloat(params.lon)   || 0,
      params.city  || "",
      parseInt(params.lowbat)  || 0,
      parseInt(params.usb)     || 0
    ]);

    return ContentService.createTextOutput("ok");
  } catch (err) {
    return ContentService.createTextOutput("error: " + err.message);
  }
}
