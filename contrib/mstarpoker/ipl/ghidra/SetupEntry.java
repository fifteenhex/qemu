import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
public class SetupEntry extends GhidraScript {
    public void run() throws Exception {
        Address ep = toAddr(0xa0000000L);
        disassemble(ep);
        createFunction(ep, "ipl_entry");
        println("SetupEntry: ipl_entry @ 0xa0000000");
    }
}
