/* LCM type definition class file
 * This file was automatically generated by lcm-gen
 * DO NOT MODIFY BY HAND!!!!
 */

package wiktor_hardware_interface;
 
import java.io.*;
import java.util.*;
import lcm.lcm.*;
 
public final class joint_path_execution_parameters implements lcm.lcm.LCMEncodable
{
    public double joint_relative_velocity;
    public double joint_relative_acceleration;
    public double override_joint_acceleration;
 
    public joint_path_execution_parameters()
    {
    }
 
    public static final long LCM_FINGERPRINT;
    public static final long LCM_FINGERPRINT_BASE = 0x31db436c85aab16cL;
 
    static {
        LCM_FINGERPRINT = _hashRecursive(new ArrayList<Class<?>>());
    }
 
    public static long _hashRecursive(ArrayList<Class<?>> classes)
    {
        if (classes.contains(wiktor_hardware_interface.joint_path_execution_parameters.class))
            return 0L;
 
        classes.add(wiktor_hardware_interface.joint_path_execution_parameters.class);
        long hash = LCM_FINGERPRINT_BASE
            ;
        classes.remove(classes.size() - 1);
        return (hash<<1) + ((hash>>63)&1);
    }
 
    public void encode(DataOutput outs) throws IOException
    {
        outs.writeLong(LCM_FINGERPRINT);
        _encodeRecursive(outs);
    }
 
    public void _encodeRecursive(DataOutput outs) throws IOException
    {
        outs.writeDouble(this.joint_relative_velocity); 
 
        outs.writeDouble(this.joint_relative_acceleration); 
 
        outs.writeDouble(this.override_joint_acceleration); 
 
    }
 
    public joint_path_execution_parameters(byte[] data) throws IOException
    {
        this(new LCMDataInputStream(data));
    }
 
    public joint_path_execution_parameters(DataInput ins) throws IOException
    {
        if (ins.readLong() != LCM_FINGERPRINT)
            throw new IOException("LCM Decode error: bad fingerprint");
 
        _decodeRecursive(ins);
    }
 
    public static wiktor_hardware_interface.joint_path_execution_parameters _decodeRecursiveFactory(DataInput ins) throws IOException
    {
        wiktor_hardware_interface.joint_path_execution_parameters o = new wiktor_hardware_interface.joint_path_execution_parameters();
        o._decodeRecursive(ins);
        return o;
    }
 
    public void _decodeRecursive(DataInput ins) throws IOException
    {
        this.joint_relative_velocity = ins.readDouble();
 
        this.joint_relative_acceleration = ins.readDouble();
 
        this.override_joint_acceleration = ins.readDouble();
 
    }
 
    public wiktor_hardware_interface.joint_path_execution_parameters copy()
    {
        wiktor_hardware_interface.joint_path_execution_parameters outobj = new wiktor_hardware_interface.joint_path_execution_parameters();
        outobj.joint_relative_velocity = this.joint_relative_velocity;
 
        outobj.joint_relative_acceleration = this.joint_relative_acceleration;
 
        outobj.override_joint_acceleration = this.override_joint_acceleration;
 
        return outobj;
    }
 
}

